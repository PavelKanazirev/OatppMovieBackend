/**
 * @file IdGeneratorTest.cpp
 * @brief Tests for identifier generation, including the concurrent case.
 */
#include "moviebackend/IdGenerator.hpp"

#include "TestSupport.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <thread>
#include <vector>

using moviebackend::IdGenerator;

namespace {

TEST(IdGeneratorTest, ProducesSequentialPrefixedIds)
{
    IdGenerator generator("m");

    EXPECT_EQ("m1", generator.next());
    EXPECT_EQ("m2", generator.next());
    EXPECT_EQ("m3", generator.next());
}

TEST(IdGeneratorTest, ObservingAnExistingIdPreventsCollisions)
{
    IdGenerator generator("s");

    generator.observeExistingId("s42");
    EXPECT_EQ("s43", generator.next());
}

TEST(IdGeneratorTest, IgnoresIdsThatCannotCollide)
{
    IdGenerator generator("m");

    generator.observeExistingId("t99");        // wrong prefix
    generator.observeExistingId("imax-hall");  // no numeric tail
    generator.observeExistingId("m");          // prefix only
    generator.observeExistingId("mABC");       // non-numeric tail

    EXPECT_EQ("m1", generator.next());
}

TEST(IdGeneratorTest, LowerExistingIdsDoNotRewindTheCounter)
{
    IdGenerator generator("b");

    generator.observeExistingId("b100");
    generator.observeExistingId("b5");

    EXPECT_EQ("b101", generator.next());
}

TEST(IdGeneratorTest, AbsurdlyLargeIdsAreIgnoredRatherThanOverflowing)
{
    IdGenerator generator("m");

    generator.observeExistingId("m99999999999999999999999999");

    EXPECT_EQ("m1", generator.next());
}

TEST(IdGeneratorTest, ConcurrentCallersNeverReceiveTheSameId)
{
    // Two request threads adding a movie at the same instant must not be
    // handed the same identifier.
    IdGenerator generator("m");

    const int threadCount = 8;
    const int idsPerThread = 500;

    std::vector<std::vector<std::string> > perThread(
        static_cast<std::size_t>(threadCount));
    std::vector<std::thread> threads;

    for (int t = 0; t < threadCount; ++t) {
        std::vector<std::string>* sink = &perThread[static_cast<std::size_t>(t)];

        // A lambda is used here because std::thread needs a callable and the
        // body is three lines - one of the few places the project allows one.
        threads.push_back(std::thread([&generator, sink, idsPerThread]() {
            for (int i = 0; i < idsPerThread; ++i) {
                sink->push_back(generator.next());
            }
        }));
    }

    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }

    std::set<std::string> unique;
    for (std::size_t t = 0; t < perThread.size(); ++t) {
        for (std::size_t i = 0; i < perThread[t].size(); ++i) {
            EXPECT_TRUE(unique.insert(perThread[t][i]).second)
                << "duplicate id: " << perThread[t][i];
        }
    }

    EXPECT_EQ(static_cast<std::size_t>(threadCount * idsPerThread), unique.size());
    EXPECT_EQ(static_cast<std::uint64_t>(threadCount * idsPerThread),
              generator.current());
}

} // unnamed namespace
