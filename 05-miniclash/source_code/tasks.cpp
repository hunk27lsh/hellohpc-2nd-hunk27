// Multi-core driver for the tasks.txt interface:
//
//    ./run tasks.txt
//
// Each line of tasks.txt names one prefix file and two output files. For every
// task, all worker threads race independent collision searches (different
// random seeds) against the same prefix; the first search to finish wins and
// its output is written, while the other workers abort at the next check_abort
// point. This keeps the wall-clock time close to (tasks / threads) * (mean
// search time) even though the search time of a single run has a long tail.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

#include "main.hpp"

void find_collision(const uint32 IV[], uint32 msg1block0[], uint32 msg1block1[],
	uint32 msg2block0[], uint32 msg2block1[], bool verbose = false);

thread_local const std::atomic<int>* g_abort_flag = nullptr;
thread_local unsigned long long g_ns_block0 = 0, g_ns_block1 = 0;

namespace {

struct block_result {
	uint32 m1block0[16];
	uint32 m1block1[16];
	uint32 m2block0[16];
	uint32 m2block1[16];
};

struct task_case {
	std::vector<unsigned char> prefix;  // zero-padded to a multiple of 64
	uint32 IV[4];
	std::string outfn1;
	std::string outfn2;
};

// Discards the progress output of the search routines.
class null_streambuf : public std::streambuf {
protected:
	int overflow(int c) override { return c; }
};

std::vector<unsigned> affinity_cpus()
{
	std::vector<unsigned> list;
#ifdef __linux__
	cpu_set_t set;
	if (sched_getaffinity(0, sizeof(set), &set) == 0)
		for (unsigned c = 0; c < CPU_SETSIZE; ++c)
			if (CPU_ISSET(c, &set))
				list.push_back(c);
#endif
	if (list.empty())
	{
		unsigned n = std::thread::hardware_concurrency();
		for (unsigned c = 0; c < n; ++c)
			list.push_back(c);
	}
	if (list.empty())
		list.push_back(0);
	return list;
}

// splitmix64 finalizer, used to derive independent seeds from one counter.
void seed_search(uint64_t x)
{
	x += 0x9e3779b97f4a7c15ull;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
	x ^= x >> 31;
	seed32_1 = uint32(x) | 1;
	seed32_2 = uint32(x >> 32) | 1;
}

void append_block(std::vector<unsigned char>& out, const uint32 block[16])
{
	for (unsigned k = 0; k < 16; ++k)
		for (unsigned c = 0; c < 4; ++c)
			out.push_back((unsigned char)((block[k] >> (c * 8)) & 0xFF));
}

bool load_case(const std::string& infn, task_case& tc)
{
	std::ifstream ifs(infn.c_str(), std::ios::binary);
	if (!ifs)
	{
		std::cerr << "cannot open input file: " << infn << std::endl;
		return false;
	}
	tc.prefix.assign(std::istreambuf_iterator<char>(ifs),
		std::istreambuf_iterator<char>());
	if (!ifs.eof() && ifs.fail())
	{
		std::cerr << "cannot read input file: " << infn << std::endl;
		return false;
	}
	while (tc.prefix.size() % 64 != 0)
		tc.prefix.push_back(0);

	uint32 IV[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
	for (std::size_t off = 0; off < tc.prefix.size(); off += 64)
	{
		uint32 block[16];
		for (unsigned k = 0; k < 16; ++k)
			block[k] = uint32(tc.prefix[off + k*4])
				| (uint32(tc.prefix[off + k*4 + 1]) << 8)
				| (uint32(tc.prefix[off + k*4 + 2]) << 16)
				| (uint32(tc.prefix[off + k*4 + 3]) << 24);
		md5_compress(IV, block);
	}
	std::memcpy(tc.IV, IV, sizeof(IV));
	return true;
}

bool write_file(const std::string& fn, const std::vector<unsigned char>& data)
{
	std::ofstream ofs(fn.c_str(), std::ios::binary | std::ios::trunc);
	if (!ofs)
	{
		std::cerr << "cannot open output file: " << fn << std::endl;
		return false;
	}
	ofs.write((const char*)&data[0], data.size());
	if (!ofs)
	{
		std::cerr << "cannot write output file: " << fn << std::endl;
		return false;
	}
	return true;
}

bool write_case(const task_case& tc, const block_result& r)
{
	std::vector<unsigned char> buf(tc.prefix);
	buf.reserve(tc.prefix.size() + 128);

	buf.resize(tc.prefix.size());
	append_block(buf, r.m1block0);
	append_block(buf, r.m1block1);
	bool ok = write_file(tc.outfn1, buf);

	buf.resize(tc.prefix.size());
	append_block(buf, r.m2block0);
	append_block(buf, r.m2block1);
	ok = write_file(tc.outfn2, buf) && ok;
	return ok;
}

std::vector<task_case> read_tasks(const char* fn)
{
	std::vector<task_case> cases;
	std::ifstream ifs(fn);
	if (!ifs)
	{
		std::cerr << "cannot read task file: " << fn << std::endl;
		return cases;
	}

	std::string line;
	while (std::getline(ifs, line))
	{
		std::istringstream ss(line);
		std::string infn, out1, out2, extra;
		if (!(ss >> infn >> out1 >> out2) || (ss >> extra))
		{
			if (!line.empty())
				std::cerr << "malformed line: " << line << std::endl;
			continue;
		}
		task_case tc;
		tc.outfn1 = out1;
		tc.outfn2 = out2;
		if (load_case(infn, tc))
			cases.push_back(std::move(tc));
	}
	return cases;
}

} // namespace

int run_tasks(const char* fn)
{
	std::vector<task_case> cases = read_tasks(fn);
	if (cases.empty())
		return 0;

	null_streambuf nullbuf;
	std::streambuf* oldbuf = std::cout.rdbuf(&nullbuf);

	const std::size_t n = cases.size();
	std::vector<std::atomic<int> > winner(n);
	for (std::size_t i = 0; i < n; ++i)
		winner[i].store(0);
	std::vector<block_result> results(n);
	std::atomic<std::size_t> next(0);
	uint64_t base_seed = 0;
	if (const char* s = std::getenv("MINICLASH_SEED"))
		base_seed = strtoull(s, 0, 10);
	std::atomic<uint64_t> attempt(base_seed);
	// MINICLASH_MODE=queue: assign one search per task (no speculation).
	const bool queue_mode =
		std::getenv("MINICLASH_MODE") && !std::strcmp(std::getenv("MINICLASH_MODE"), "queue");

	const bool trace = std::getenv("MINICLASH_TRACE") != 0;
	std::atomic<unsigned long long> ns_block0(0), ns_block1(0);
	const std::chrono::steady_clock::time_point t0 =
		std::chrono::steady_clock::now();
	struct task_stat {
		std::atomic<long long> attempts;
		std::atomic<long long> done_ms;
	};
	std::vector<task_stat> stat(n);
	for (std::size_t i = 0; i < n; ++i)
	{
		stat[i].attempts.store(0);
		stat[i].done_ms.store(-1);
	}

	const std::vector<unsigned> cpus = affinity_cpus();
	const unsigned nthreads = (unsigned)cpus.size();

	std::vector<std::thread> threads;
	threads.reserve(nthreads);
	for (unsigned t = 0; t < nthreads; ++t)
	{
		threads.push_back(std::thread([&, t]() {
#ifdef __linux__
			// one worker per available CPU: no migration, stable caches
			{
				cpu_set_t set;
				CPU_ZERO(&set);
				CPU_SET(cpus[t % cpus.size()], &set);
				pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
			}
#endif
			for (;;)
			{
				std::size_t i;
				if (queue_mode)
				{
					i = next.fetch_add(1);
					if (i >= n)
						break;
				}
				else
				{
					i = next.load(std::memory_order_acquire);
					if (i >= n)
						break;
					if (winner[i].load(std::memory_order_relaxed) != 0)
					{
						std::size_t expected = i;
						next.compare_exchange_strong(expected, i + 1);
						continue;
					}
				}

				block_result local;
				seed_search(attempt.fetch_add(1) + 1);
				if (trace)
					stat[i].attempts.fetch_add(1);
				g_abort_flag = &winner[i];
				bool found = true;
				try
				{
					find_collision(cases[i].IV, local.m1block0, local.m1block1,
						local.m2block0, local.m2block1, false);
				}
				catch (search_aborted&)
				{
					found = false;
				}
				g_abort_flag = nullptr;
				if (!found)
					continue;

				int expected = 0;
				if (winner[i].compare_exchange_strong(expected, 1))
				{
					results[i] = local;
					if (trace)
						stat[i].done_ms.store(std::chrono::duration_cast<
							std::chrono::milliseconds>(std::chrono::steady_clock::now()
								- t0).count());
					if (!write_case(cases[i], results[i]))
						std::cerr << "task " << i << ": failed to write output"
							<< std::endl;
					if (!queue_mode)
					{
						std::size_t exp = i;
						next.compare_exchange_strong(exp, i + 1);
					}
				}
			}
			ns_block0.fetch_add(g_ns_block0, std::memory_order_relaxed);
			ns_block1.fetch_add(g_ns_block1, std::memory_order_relaxed);
		}));
	}
	for (unsigned t = 0; t < threads.size(); ++t)
		threads[t].join();

	if (trace)
	{
		std::cerr << "tasks=" << n << " threads=" << nthreads
			<< " block0_s=" << (ns_block0.load() / 1e9)
			<< " block1_s=" << (ns_block1.load() / 1e9) << std::endl;
		for (std::size_t i = 0; i < n; ++i)
			std::cerr << "task " << i << " attempts=" << stat[i].attempts.load()
				<< " done_ms=" << stat[i].done_ms.load() << std::endl;
	}

	std::cout.rdbuf(oldbuf);
	return 0;
}
