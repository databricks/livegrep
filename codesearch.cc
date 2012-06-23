#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <locale>
#include <list>
#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <limits>

#include <re2/re2.h>

#include <gflags/gflags.h>

#include "timer.h"
#include "thread_queue.h"
#include "thread_pool.h"
#include "codesearch.h"
#include "chunk.h"
#include "chunk_allocator.h"
#include "radix_sort.h"
#include "atomic.h"
#include "indexer.h"
#include "per_thread.h"

#include "utf8.h"

using re2::RE2;
using re2::StringPiece;
using namespace std;

const int    kContextLines = 3;

const size_t kMinSkip = 250;
const int kMinFilterRatio = 50;
const int kMaxScan        = (1 << 20);

#ifdef PROFILE_CODESEARCH
DEFINE_bool(debug_search, false, "Produce debugging output about the search process");
#define log_profile(format, ...) do {                   \
        if (FLAGS_debug_search)                         \
            fprintf(stderr, format, ## __VA_ARGS__);    \
    } while(0)
#else
#define log_profile(...)
#endif

DEFINE_bool(index, true, "Create a suffix-array index to speed searches.");
DEFINE_bool(search, true, "Actually do the search.");
DEFINE_int32(max_matches, 50, "The maximum number of results to return for a single query.");
DEFINE_int32(timeout, 1, "The number of seconds a single search may run for.");
DEFINE_string(order_root, "", "Walk top-level directories in this order.");
DECLARE_int32(threads);

bool eqstr::operator()(const StringPiece& lhs, const StringPiece& rhs) const {
    if (lhs.data() == NULL && rhs.data() == NULL)
        return true;
    if (lhs.data() == NULL || rhs.data() == NULL)
        return false;
    return lhs == rhs;
}

size_t hashstr::operator()(const StringPiece& str) const {
    const std::collate<char>& coll = std::use_facet<std::collate<char> >(loc);
    return coll.hash(str.data(), str.data() + str.size());
}

bool operator==(const git_oid &lhs, const git_oid &rhs) {
    return memcmp(lhs.id, rhs.id, GIT_OID_RAWSZ) == 0;
}

size_t hashoid::operator()(const git_oid& oid) const {
    /*
     * We could hash the entire oid together, but since the oid is the
     * output of a cryptographic hash anyways, just taking the first N
     * bytes should work just well.
     */
    union {
        git_oid oid;
        size_t size;
    } u = {oid};
    return u.size;
}

const StringPiece empty_string(NULL, 0);

class code_searcher;
struct match_finger;
struct match_group;

class searcher {
public:
    searcher(const code_searcher *cc, thread_queue<match_result*>& queue,
             RE2& pat, RE2 *file_pat) :
        cc_(cc), pat_(pat), file_pat_(file_pat), queue_(queue),
        matches_(0), re2_time_(false), git_time_(false),
        index_time_(false), sort_time_(false), analyze_time_(false),
        exit_reason_(kExitNone), files_(new uint8_t[cc->files_.size()]),
        files_density_(-1)
    {
        memset(files_, 0xff, cc->files_.size());
        {
            run_timer run(analyze_time_);
            index_ = indexRE(pat);
        }

        if (FLAGS_timeout <= 0) {
            limit_.tv_sec = numeric_limits<time_t>::max();
        } else {
            gettimeofday(&limit_, NULL);
            limit_.tv_sec += FLAGS_timeout;
        }
    }

    ~searcher() {
        delete[] files_;

        log_profile("re2 time: %d.%06ds\n",
                    int(re2_time_.elapsed().tv_sec),
                    int(re2_time_.elapsed().tv_usec));
        log_profile("git time: %d.%06ds\n",
                    int(git_time_.elapsed().tv_sec),
                    int(git_time_.elapsed().tv_usec));
        log_profile("index time: %d.%06ds\n",
                    int(index_time_.elapsed().tv_sec),
                    int(index_time_.elapsed().tv_usec));
        log_profile("sort time: %d.%06ds\n",
                    int(sort_time_.elapsed().tv_sec),
                    int(sort_time_.elapsed().tv_usec));
        log_profile("analyze time: %d.%06ds\n",
                    int(analyze_time_.elapsed().tv_sec),
                    int(analyze_time_.elapsed().tv_usec));
    }

    void operator()(const chunk *chunk);

    void get_stats(match_stats *stats) {
        stats->re2_time = re2_time_.elapsed();
        stats->git_time = git_time_.elapsed();
        stats->index_time = index_time_.elapsed();
        stats->sort_time  = sort_time_.elapsed();
        stats->analyze_time  = analyze_time_.elapsed();
    }

    exit_reason why() {
        return exit_reason_;
    }

protected:
    void next_range(match_finger *finger, int& minpos, int& maxpos, int end);
    void full_search(const chunk *chunk);
    void full_search(match_finger *finger, const chunk *chunk,
                     size_t minpos, size_t maxpos);

    void filtered_search(const chunk *chunk);
    void search_lines(uint32_t *left, int count, const chunk *chunk);

    bool accept(const git_path &path) {
        if (!file_pat_)
            return true;
        return file_pat_->Match(path.path, 0, path.path.size(),
                                RE2::UNANCHORED, 0, 0);
    }

    bool accept(search_file *sf) {
        if (!file_pat_)
            return true;

        assert(cc_->files_[sf->no] == sf);

        if (files_[sf->no] == 0xff) {
            bool match = 0;
            for (auto it = sf->paths.begin(); it != sf->paths.end(); ++it) {
                if (accept(*it)) {
                    match = true;
                    break;
                }
            }
            files_[sf->no] = match;
        }

        return files_[sf->no];
    }

    bool accept(const list<search_file *> &sfs) {
        for (list<search_file *>::const_iterator it = sfs.begin();
             it != sfs.end(); ++it) {
            if (accept(*it))
                return true;
        }
        return false;
    }

    double files_density(void) {
        mutex_locker locked(mtx_);
        if (files_density_ >= 0)
            return files_density_;

        int hits = 0;
        int sample = min(1000, int(cc_->files_.size()));
        for (int i = 0; i < sample; i++) {
            if (accept(cc_->files_[rand() % cc_->files_.size()]))
                hits++;
        }
        return (files_density_ = double(hits) / sample);
    }

    /*
     * Do a linear walk over chunk->files, searching for all files
     * which contain `match', which is contained within `line'.
     */
    void find_match_brute(const chunk *chunk,
                          const StringPiece& match,
                          const StringPiece& line);

    /*
     * Given a match `match', contained within `line', find all files
     * that contain that match. If indexing is enabled, do this by
     * walking the chunk_file BST; Otherwise, fall back on a
     * brute-force linear walk.
     */
    void find_match(const chunk *chunk,
                    const StringPiece& match,
                    const StringPiece& line);

    /*
     * Given a matching substring, its containing line, and a search
     * file, determine whether that file actually contains that line,
     * and if so, post a match to the result queue.
     */
    void try_match(match_group *,
                   const StringPiece&,
                   const StringPiece&,
                   search_file *);

    void finish_group(match_group *);

    static int line_start(const chunk *chunk, int pos) {
        const unsigned char *start = static_cast<const unsigned char*>
            (memrchr(chunk->data, '\n', pos));
        if (start == NULL)
            return 0;
        return start - chunk->data;
    }

    static int line_end(const chunk *chunk, int pos) {
        const unsigned char *end = static_cast<const unsigned char*>
            (memchr(chunk->data + pos, '\n', chunk->size - pos));
        if (end == NULL)
            return chunk->size;
        return end - chunk->data;
    }

    static StringPiece find_line(const StringPiece& chunk, const StringPiece& match) {
        const char *start, *end;
        assert(match.data() >= chunk.data());
        assert(match.data() <= chunk.data() + chunk.size());
        assert(match.size() <= (chunk.size() - (match.data() - chunk.data())));
        start = static_cast<const char*>
            (memrchr(chunk.data(), '\n', match.data() - chunk.data()));
        if (start == NULL)
            start = chunk.data();
        else
            start++;
        end = static_cast<const char*>
            (memchr(match.data() + match.size(), '\n',
                    chunk.size() - (match.data() - chunk.data()) - match.size()));
        if (end == NULL)
            end = chunk.data() + chunk.size();
        return StringPiece(start, end - start);
    }

    bool exit_early() {
        if (exit_reason_)
            return true;

        if (matches_.load() >= FLAGS_max_matches) {
            exit_reason_ = kExitMatchLimit;
            return true;
        }
#ifdef CODESEARCH_SLOWGTOD
        static int counter = 1000;
        if (--counter)
            return false;
        counter = 1000;
#endif
        timeval now;
        gettimeofday(&now, NULL);
        if (now.tv_sec > limit_.tv_sec ||
            (now.tv_sec == limit_.tv_sec && now.tv_usec > limit_.tv_usec)) {
            exit_reason_ = kExitTimeout;
            return true;
        }
        return false;
    }

    const code_searcher *cc_;
    RE2& pat_;
    RE2 *file_pat_;
    thread_queue<match_result*>& queue_;
    atomic_int matches_;
    intrusive_ptr<IndexKey> index_;
    timer re2_time_;
    timer git_time_;
    timer index_time_;
    timer sort_time_;
    timer analyze_time_;
    timeval limit_;
    exit_reason exit_reason_;
    uint8_t *files_;

    /*
     * The approximate ratio of how many files match file_pat_. Lazily
     * computed -- -1 means it hasn't been computed yet. Protected by
     * mtx_.
     */
    double files_density_;
    cs_mutex mtx_;

    friend class code_searcher::search_thread;
};

code_searcher::code_searcher()
    : stats_(), alloc_(0), finalized_(false)
{
#ifdef USE_DENSE_HASH_SET
    lines_.set_empty_key(empty_string);
#endif
}

void code_searcher::set_alloc(chunk_allocator *alloc) {
    assert(!alloc_);
    alloc_ = alloc;
}

code_searcher::~code_searcher() {
    if (alloc_)
        alloc_->cleanup();
    delete alloc_;
}

namespace {
    void resolve_ref(git_repository *repo,
                     smart_object<git_commit>& out,
                     const char *refname) {
        git_reference *ref;
        const git_oid *oid;
        git_oid tmp;
        smart_object<git_object> obj;
        if (git_oid_fromstr(&tmp, refname) == GIT_SUCCESS) {
            git_object_lookup(obj, repo, &tmp, GIT_OBJ_ANY);
        } else {
            git_reference_lookup(&ref, repo, refname);
            git_reference_resolve(&ref, ref);
            oid = git_reference_oid(ref);
            git_object_lookup(obj, repo, oid, GIT_OBJ_ANY);
        }
        if (git_object_type(obj) == GIT_OBJ_TAG) {
            git_tag_target(out, obj);
        } else {
            out = obj.release();
        }
    }
};

void code_searcher::walk_ref(git_repository *repo, const char *ref) {
    assert(alloc_);
    assert(!finalized_);
    smart_object<git_commit> commit;
    smart_object<git_tree> tree;
    resolve_ref(repo, commit, ref);
    git_commit_tree(tree, commit);

    refs_.push_back(ref);

    walk_root(repo, ref, tree);
}

void code_searcher::walk_root(git_repository *repo, const char *ref, git_tree *tree) {
    map<string, const git_tree_entry *> root;
    vector<const git_tree_entry *> ordered;
    int entries = git_tree_entrycount(tree);
    for (int i = 0; i < entries; ++i) {
        const git_tree_entry *ent = git_tree_entry_byindex(tree, i);
        root[git_tree_entry_name(ent)] = ent;
    }

    istringstream stream(FLAGS_order_root);
    string dir;
    while(stream >> dir) {
        map<string, const git_tree_entry *>::iterator it = root.find(dir);
        if (it == root.end())
            continue;
        ordered.push_back(it->second);
        root.erase(it);
    }
    for (map<string, const git_tree_entry *>::iterator it = root.begin();
         it != root.end(); ++it)
        ordered.push_back(it->second);
    for (vector<const git_tree_entry *>::iterator it = ordered.begin();
         it != ordered.end(); ++it) {
        smart_object<git_object> obj;
        git_tree_entry_2object(obj, repo, *it);
        string path = string(git_tree_entry_name(*it));

        if (git_tree_entry_type(*it) == GIT_OBJ_TREE) {
            walk_tree(repo, ref, path + "/", obj);
        } else if (git_tree_entry_type(*it) == GIT_OBJ_BLOB) {
            update_stats(ref, path, obj);
        }
    }
}

void code_searcher::dump_stats() {
    log_profile("chunk_files: %d\n", chunk::chunk_files);
    printf("Bytes: %ld (dedup: %ld)\n", stats_.bytes, stats_.dedup_bytes);
    printf("Lines: %ld (dedup: %ld)\n", stats_.lines, stats_.dedup_lines);
    printf("Files: %ld (dedup: %ld)\n", stats_.files, stats_.dedup_files);
}

void code_searcher::finalize() {
    assert(!finalized_);
    finalized_ = true;
    alloc_->finalize();
}

void code_searcher::walk_tree(git_repository *repo,
                              const char *ref,
                              const string& pfx,
                              git_tree *tree) {
    string path;
    int entries = git_tree_entrycount(tree);
    int i;
    for (i = 0; i < entries; i++) {
        const git_tree_entry *ent = git_tree_entry_byindex(tree, i);
        path = pfx + git_tree_entry_name(ent);
        smart_object<git_object> obj;
        git_tree_entry_2object(obj, repo, ent);
        if (git_tree_entry_type(ent) == GIT_OBJ_TREE) {
            walk_tree(repo, ref, path + "/", obj);
        } else if (git_tree_entry_type(ent) == GIT_OBJ_BLOB) {
            update_stats(ref, path, obj);
        }
    }
}

void code_searcher::update_stats(const char *ref, const string& path, git_blob *blob) {
    size_t len = git_blob_rawsize(blob);
    const char *p = static_cast<const char*>(git_blob_rawcontent(blob));
    const char *end = p + len;
    const char *f;
    chunk *c;
    StringPiece line;

    if (memchr(p, 0, len) != NULL)
        return;

    stats_.bytes += len;
    stats_.files++;

    const git_oid *oid = git_object_id(reinterpret_cast<git_object*>(blob));
    auto sit = file_map_.find(*oid);
    if (sit != file_map_.end()) {
        search_file *sf = sit->second;
        sf->paths.push_back((git_path){ref, path});
        return;
    }

    stats_.dedup_files++;

    search_file *sf = new search_file;
    sf->paths.push_back((git_path){ref, path});
    git_oid_cpy(&sf->oid, oid);
    sf->no  = files_.size();
    files_.push_back(sf);
    file_map_[*oid] = sf;

    while ((f = static_cast<const char*>(memchr(p, '\n', end - p))) != 0) {
        string_hash::iterator it = lines_.find(StringPiece(p, f - p));
        if (it == lines_.end()) {
            stats_.dedup_bytes += (f - p) + 1;
            stats_.dedup_lines ++;

            // Include the trailing '\n' in the chunk buffer
            unsigned char *alloc = alloc_->alloc(f - p + 1);
            memcpy(alloc, p, f - p + 1);
            line = StringPiece((char*)alloc, f - p);
            lines_.insert(line);
            c = alloc_->current_chunk();
        } else {
            line = *it;
            c = alloc_->chunk_from_string
                (reinterpret_cast<const unsigned char*>(line.data()));
        }
        c->add_chunk_file(sf, line);
        if (sf->content.size() &&
            sf->content.back().data() +
            sf->content.back().size() == line.data()) {
            StringPiece &back = sf->content.back();
            assert(back.data()[back.size()] == '\n');
            back = StringPiece(back.data(),
                               (line.data() - back.data() + line.size()));
        } else {
            sf->content.push_back(StringPiece(line.data(), line.size()));
        }
        p = f + 1;
        stats_.lines++;
    }

    for (auto it = alloc_->begin();
         it != alloc_->end(); it++)
        (*it)->finish_file();
}

void searcher::operator()(const chunk *chunk)
{
    if (exit_reason_)
        return;

    if (FLAGS_index && index_ && !index_->empty())
        filtered_search(chunk);
    else
        full_search(chunk);
}

struct walk_state {
    uint32_t *left, *right;
    intrusive_ptr<IndexKey> key;
    int depth;
};

struct lt_index {
    const chunk *chunk_;
    int idx_;

    bool operator()(uint32_t lhs, unsigned char rhs) {
        return cmp(lhs, rhs) < 0;
    }

    bool operator()(unsigned char lhs, uint32_t rhs) {
        return cmp(rhs, lhs) > 0;
    }

    int cmp(uint32_t lhs, unsigned char rhs) {
        unsigned char lc = chunk_->data[lhs + idx_];
        if (lc == '\n')
            return -1;
        return (int)lc - (int)rhs;
    }
};


void searcher::filtered_search(const chunk *chunk)
{
    static per_thread<vector<uint32_t> > indexes;
    if (!indexes.get()) {
        indexes.put(new vector<uint32_t>(kChunkSize / kMinFilterRatio));
    }
    int count = 0;
    {
        run_timer run(index_time_);
        vector<walk_state> stack;
        stack.push_back((walk_state){
                chunk->suffixes, chunk->suffixes + chunk->size, index_, 0});

        while (!stack.empty()) {
            walk_state st = stack.back();
            stack.pop_back();
            if (!st.key || st.key->empty() || (st.right - st.left) <= 100) {
                if ((count + st.right - st.left) > indexes->size()) {
                    count = indexes->size() + 1;
                    break;
                }
                memcpy(&(*indexes)[count], st.left,
                       (st.right - st.left) * sizeof(uint32_t));
                count += (st.right - st.left);
                continue;
            }
            lt_index lt = {chunk, st.depth};
            for (IndexKey::iterator it = st.key->begin();
                 it != st.key->end(); ++it) {
                uint32_t *l, *r;
                l = lower_bound(st.left, st.right, it->first.first, lt);
                uint32_t *right = lower_bound(l, st.right,
                                              (unsigned char)(it->first.second + 1),
                                              lt);
                if (l == right)
                    continue;

                if (st.depth)
                    assert(chunk->data[*l + st.depth - 1] ==
                           chunk->data[*(right - 1) + st.depth - 1]);

                assert(l == st.left ||
                       chunk->data[*(l-1) + st.depth] == '\n' ||
                       chunk->data[*(l-1) + st.depth] < it->first.first);
                assert(chunk->data[*l + st.depth] >= it->first.first);
                assert(right == st.right ||
                       chunk->data[*right + st.depth] > it->first.second);

                for (unsigned char ch = it->first.first; ch <= it->first.second;
                     ch++, l = r) {
                    r = lower_bound(l, right, (unsigned char)(ch + 1), lt);

                    if (r != l) {
                        stack.push_back((walk_state){l, r, it->second, st.depth + 1});
                    }
                }
            }
        }
    }

    search_lines(&(*indexes)[0], count, chunk);
}

struct match_finger {
    const chunk *chunk_;
    vector<chunk_file>::const_iterator it_;
    match_finger(const chunk *chunk) :
        chunk_(chunk), it_(chunk->files.begin()) {};
};

void searcher::search_lines(uint32_t *indexes, int count,
                            const chunk *chunk)
{
    log_profile("search_lines: Searching %d/%d indexes.\n", count, chunk->size);

    if (count == 0)
        return;

    if (count * kMinFilterRatio > chunk->size) {
        full_search(chunk);
        return;
    }

    if (file_pat_ && double(count * 30) / chunk->size > files_density()) {
        full_search(chunk);
        return;
    }

    {
        run_timer run(sort_time_);
        lsd_radix_sort(indexes, indexes + count);
    }

    match_finger finger(chunk);

    StringPiece search((char*)chunk->data, chunk->size);
    uint32_t max = indexes[0];
    uint32_t min = line_start(chunk, indexes[0]);
    for (int i = 0; i <= count && !exit_early(); i++) {
        if (i != count) {
            if (indexes[i] < max) continue;
            if (indexes[i] < max + kMinSkip) {
                max = indexes[i];
                continue;
            }
        }

        int end = line_end(chunk, max);
        full_search(&finger, chunk, min, end);

        if (i != count) {
            max = indexes[i];
            min = line_start(chunk, max);
        }
    }
}

void searcher::full_search(const chunk *chunk)
{
    match_finger finger(chunk);
    full_search(&finger, chunk, 0, chunk->size - 1);
}

void searcher::next_range(match_finger *finger,
                          int& pos, int& endpos, int maxpos)
{
    if (!file_pat_ || !FLAGS_index)
        return;

    log_profile("next_range(%d, %d, %d)\n", pos, endpos, maxpos);

    vector<chunk_file>::const_iterator& it = finger->it_;
    const vector<chunk_file>::const_iterator& end = finger->chunk_->files.end();

    /* Find the first matching range that intersects [pos, maxpos) */
    while (it != end &&
           (it->right < pos || !accept(it->files)) &&
           it->left < maxpos)
        ++it;

    if (it == end || it->left >= maxpos) {
        pos = endpos = maxpos;
        return;
    }

    pos    = max(pos, it->left);
    endpos = it->right;

    /*
     * Now scan until we either:
     * - prove that [pos, maxpos) is all in range,
     * - find a gap greater than kMinSkip, or
     * - pass maxpos entirely.
     */
    do {
        if (it->left >= endpos + kMinSkip)
            break;
        if (it->right >= endpos && accept(it->files)) {
            endpos = max(endpos, it->right);
            if (endpos >= maxpos)
                /*
                 * We've accepted the entire range. No point in going on.
                 */
                break;
        }
        ++it;
    } while (it != end && it->left < maxpos);

    endpos = min(endpos, maxpos);
}

void searcher::full_search(match_finger *finger,
                           const chunk *chunk, size_t minpos, size_t maxpos)
{
    StringPiece str((char*)chunk->data, chunk->size);
    StringPiece match;
    int pos = minpos, new_pos, end = minpos;
    while (pos < maxpos && !exit_early()) {
        if (pos >= end) {
            end = maxpos;
            next_range(finger, pos, end, maxpos);
            assert(pos <= end);
        }
        if (pos >= maxpos)
            break;

        log_profile("[%p] range:%d-%d/%d-%d\n",
                    (void*)(chunk), pos, end, int(minpos), int(maxpos));

        {
            int limit = end;
            if (limit - pos > kMaxScan)
                limit = line_end(chunk, pos + kMaxScan);
            run_timer run(re2_time_);
            if (!pat_.Match(str, pos, limit, RE2::UNANCHORED, &match, 1)) {
                pos = limit + 1;
                continue;
            }
        }
        assert(memchr(match.data(), '\n', match.size()) == NULL);
        StringPiece line = find_line(str, match);
        if (utf8::is_valid(line.data(), line.data() + line.size()))
            find_match(chunk, match, line);
        new_pos = line.size() + line.data() - str.data() + 1;
        assert(new_pos > pos);
        pos = new_pos;
    }
}

struct match_group {
    StringPiece match, line;
    int left, right;
    map<string, vector<match_context> > matches;

    match_group(const StringPiece& match, const StringPiece &line)
        : match(match), line(line) {
        left = utf8::distance(line.data(), match.data());
        right = left +
            utf8::distance(match.data(), match.data() + match.size());
    }
};

void searcher::find_match_brute(const chunk *chunk,
                                const StringPiece& match,
                                const StringPiece& line) {
    run_timer run(git_time_);
    timer tm;
    int off = (unsigned char*)line.data() - chunk->data;
    int searched = 0;
    match_group group(match, line);

    for(vector<chunk_file>::const_iterator it = chunk->files.begin();
        it != chunk->files.end(); it++) {
        if (off >= it->left && off <= it->right) {
            for (list<search_file *>::const_iterator fit = it->files.begin();
                 fit != it->files.end(); ++fit) {
                if (!accept(*fit))
                    continue;
                searched++;
                if (exit_early())
                    break;
                try_match(&group, line, match, *fit);
            }
        }
    }

    finish_group(&group);

    tm.pause();
    log_profile("Searched %d files in %d.%06ds\n",
                searched,
                int(tm.elapsed().tv_sec),
                int(tm.elapsed().tv_usec));
}

void searcher::find_match(const chunk *chunk,
                          const StringPiece& match,
                          const StringPiece& line) {
    if (!FLAGS_index) {
        find_match_brute(chunk, match, line);
        return;
    }

    run_timer run(git_time_);
    int loff = (unsigned char*)line.data() - chunk->data;
    match_group group(match, line);

    /*
     * We use an explicit stack instead of direct recursion. We
     * want to do an inorder traversal, so that we produce results
     * in ascending order of position in the chunk, so we have two
     * types of frames we can push onto the stack.
     *
     * A frame with visit = false means that this is the initial
     * visit to 'node', and we should inspect its position and
     * push its children, if appropriate. If the node itself is
     * worth searching, we also push the node again, with visit =
     * true, in between the children.
     *
     * When we encounter a node with visit=true, we actually scan
     * it for matches.
     *
     */

    struct frame {
        chunk_file_node *node;
        bool visit;
    };

    vector<frame> stack;
    stack.push_back((frame){chunk->cf_root, false});

    log_profile("find_match(%d)\n", loff);

    while (!stack.empty() && !exit_reason_) {
        frame f = stack.back();
        stack.pop_back();

        chunk_file_node *n = f.node;

        if (f.visit) {
            log_profile("visit <%d-%d>\n", n->chunk->left, n->chunk->right);
            assert(loff >= n->chunk->left && loff <= n->chunk->right);
            for (list<search_file *>::const_iterator it = n->chunk->files.begin();
                 it != n->chunk->files.end(); ++it) {
                if (!accept(*it))
                    continue;
                if (exit_early())
                    break;
                try_match(&group, line, match, *it);
            }
            continue;
        }

        log_profile("walk <%d-%d> - %d\n", n->chunk->left, n->chunk->right,
                    n->right_limit);

        if (loff > n->right_limit)
            continue;
        if (loff >= n->chunk->left) {
            if (n->right)
                stack.push_back((frame){n->right, false});
            if (loff <= n->chunk->right)
                stack.push_back((frame){n, true});
        }
        if (n->left)
            stack.push_back((frame){n->left, false});
    }
    finish_group(&group);
}


void searcher::try_match(match_group *group,
                         const StringPiece& line,
                         const StringPiece& match,
                         search_file *sf) {

    int lno = 1;
    vector<StringPiece>::iterator it;
    for (it = sf->content.begin();
         it != sf->content.end(); ++it) {
        if (line.data() >= it->data() &&
            line.data() <= it->data() + it->size()) {
            lno += count(it->data(), line.data(), '\n');
            break;
        } else {
            lno += count(it->data(), it->data() + it->size(), '\n') + 1;
        }
    }

    if (it == sf->content.end())
        return;

    match_context ctx;

    ctx.file = sf;
    ctx.lno  = lno;

    vector<StringPiece>::iterator mit = it;
    StringPiece l = line;
    int i = 0;

    for (i = 0; i < kContextLines; i++) {
        if (l.data() == it->data()) {
            if (it == sf->content.begin())
                break;
            --it;
            l = StringPiece(it->data() + it->size() + 1, 0);
        }
        l = find_line(*it, StringPiece(l.data() - 1, 0));
        ctx.context_before.push_back(l);
    }

    l = line;
    it = mit;
    for (i = 0; i < kContextLines; i++) {
        if (l.data() + l.size() == it->data() + it->size()) {
            if (++it == sf->content.end())
                break;
            l = StringPiece(it->data() - 1, 0);
        }
        l = find_line(*it, StringPiece(l.data() + l.size() + 1, 0));
        ctx.context_after.push_back(l);
    }

    for (auto it = sf->paths.begin(); it != sf->paths.end(); ++it) {
        if (!accept(*it))
            continue;
        auto git = group->matches.find(it->path);
        if (git == group->matches.end()) {
            ++matches_;
            group->matches[it->path] = vector<match_context>();
            group->matches[it->path].push_back(ctx);
        } else if (git->second.back().file != sf) {
            git->second.push_back(ctx);
        }
        group->matches[it->path].back().paths.push_back(*it);
    }
}

void searcher::finish_group(match_group *group) {
    for (auto it = group->matches.begin(); it != group->matches.end(); ++it) {
        match_result *m = new match_result;
        m->line       = group->line;
        m->matchleft  = group->left;
        m->matchright = group->right;
        m->context.swap(it->second);
        queue_.push(m);
    }
}

code_searcher::search_thread::search_thread(code_searcher *cs)
    : cs_(cs), pool_(FLAGS_threads, &search_one) {
}

void code_searcher::search_thread::match_internal(RE2& pat, RE2 *file_pat,
                                                 const code_searcher::search_thread::base_cb& cb,
                                                 match_stats *stats) {
    match_result *m;
    int matches = 0;
    int pending = cs_->alloc_->size();

    assert(cs_->finalized_);

    thread_queue<match_result*> results;
    searcher search(cs_, results, pat, file_pat);

    memset(stats, 0, sizeof *stats);
    stats->why = kExitNone;

    if (!FLAGS_search) {
        return;
    }


    for (auto it = cs_->alloc_->begin(); it != cs_->alloc_->end(); it++) {
        pool_.queue(pair<searcher*, chunk*>(&search, *it));
    }

    while (pending) {
        m = results.pop();
        if (!m) {
            pending--;
            continue;
        }
        matches++;
        cb(m);
        delete m;
    }

    search.get_stats(stats);
    stats->why = search.why();
    stats->matches = matches;
}


code_searcher::search_thread::~search_thread() {
    for (int i = 0; i < FLAGS_threads; i++)
        pool_.queue(pair<searcher*, chunk*>((searcher*)0, (chunk*)0));
}

bool code_searcher::search_thread::search_one(const pair<searcher*, chunk*>& pair) {
    if (!pair.first)
        return true;
    (*pair.first)(pair.second);
    pair.first->queue_.push(NULL);
    return false;
}
