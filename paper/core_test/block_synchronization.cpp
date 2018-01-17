#include <gtest/gtest.h>
#include <paper/node.hpp>

TEST (pull_synchronization, empty)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	std::vector <std::unique_ptr <paper::block>> blocks;
	paper::pull_synchronization sync ([&blocks] (paper::block const & block_a)
	{
		blocks.push_back (block_a.clone ());
	}, store);
	ASSERT_TRUE (sync.synchronize (0));
	ASSERT_EQ (0, blocks.size ());
}

TEST (pull_synchronization, one)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::open_block block1 (0, 1, 2, 3, 4, 5);
	paper::send_block block2 (block1.hash (), 0, 1, 2, 3, 4);
	std::vector <std::unique_ptr <paper::block>> blocks;
	{
		paper::transaction transaction (store.environment, nullptr, true);
		store.block_put (transaction, block1.hash (), block1);
		store.unchecked_put (transaction, block2.hash (), block2);
	}
	paper::pull_synchronization sync ([&blocks] (paper::block const & block_a)
	{
		blocks.push_back (block_a.clone ());
	}, store);
	ASSERT_FALSE (sync.synchronize (block2.hash ()));
	ASSERT_EQ (1, blocks.size ());
	ASSERT_EQ (block2, *blocks [0]);
}

TEST (pull_synchronization, send_dependencies)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::open_block block1 (0, 1, 2, 3, 4, 5);
	paper::send_block block2 (block1.hash (), 0, 1, 2, 3, 4);
	paper::send_block block3 (block2.hash (), 0, 1, 2, 3, 4);
	std::vector <std::unique_ptr <paper::block>> blocks;
	{
		paper::transaction transaction (store.environment, nullptr, true);
		store.block_put (transaction, block1.hash (), block1);
		store.unchecked_put (transaction, block2.hash (), block2);
		store.unchecked_put (transaction, block3.hash (), block3);
	}
	paper::pull_synchronization sync ([&blocks, &store] (paper::block const & block_a)
									{
										paper::transaction transaction (store.environment, nullptr, true);
										store.block_put (transaction, block_a.hash (), block_a);
										blocks.push_back (block_a.clone ());
									}, store);
	ASSERT_FALSE (sync.synchronize (block3.hash ()));
	ASSERT_EQ (2, blocks.size ());
	ASSERT_EQ (block2, *blocks [0]);
	ASSERT_EQ (block3, *blocks [1]);
}

TEST (pull_synchronization, change_dependencies)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::open_block block1 (0, 1, 2, 3, 4, 5);
	paper::send_block block2 (block1.hash (), 0, 1, 2, 3, 4);
	paper::change_block block3 (block2.hash (), 0, 1, 2, 3);
	std::vector <std::unique_ptr <paper::block>> blocks;
	{
		paper::transaction transaction (store.environment, nullptr, true);
		store.block_put (transaction, block1.hash (), block1);
		store.unchecked_put (transaction, block2.hash (), block2);
		store.unchecked_put (transaction, block3.hash (), block3);
	}
	paper::pull_synchronization sync ([&blocks, &store] (paper::block const & block_a)
									{
										paper::transaction transaction (store.environment, nullptr, true);
										store.block_put (transaction, block_a.hash (), block_a);
										blocks.push_back (block_a.clone ());
									}, store);
	ASSERT_FALSE (sync.synchronize (block3.hash ()));
	ASSERT_EQ (2, blocks.size ());
	ASSERT_EQ (block2, *blocks [0]);
	ASSERT_EQ (block3, *blocks [1]);
}

TEST (pull_synchronization, open_dependencies)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::open_block block1 (0, 1, 2, 3, 4, 5);
	paper::send_block block2 (block1.hash (), 0, 1, 2, 3, 4);
	paper::open_block block3 (block2.hash (), 1, 1, 3, 4, 5);
	std::vector <std::unique_ptr <paper::block>> blocks;
	{
		paper::transaction transaction (store.environment, nullptr, true);
		store.block_put (transaction, block1.hash (), block1);
		store.unchecked_put (transaction, block2.hash (), block2);
		store.unchecked_put (transaction, block3.hash (), block3);
	}
	paper::pull_synchronization sync ([&blocks, &store] (paper::block const & block_a)
									{
										paper::transaction transaction (store.environment, nullptr, true);
										store.block_put (transaction, block_a.hash (), block_a);
										blocks.push_back (block_a.clone ());
									}, store);
	ASSERT_FALSE (sync.synchronize (block3.hash ()));
	ASSERT_EQ (2, blocks.size ());
	ASSERT_EQ (block2, *blocks [0]);
	ASSERT_EQ (block3, *blocks [1]);
}

TEST (pull_synchronization, receive_dependencies)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::open_block block1 (0, 1, 2, 3, 4, 5);
	paper::send_block block2 (block1.hash (), 0, 1, 2, 3, 4);
	paper::open_block block3 (block2.hash (), 1, 1, 3, 4, 5);
	paper::send_block block4 (block2.hash (), 0, 1, 2, 3, 4);
	paper::receive_block block5 (block3.hash (), block4.hash (), 0, 0, 0);
	std::vector <std::unique_ptr <paper::block>> blocks;
	{
		paper::transaction transaction (store.environment, nullptr, true);
		store.block_put (transaction, block1.hash (), block1);
		store.unchecked_put (transaction, block2.hash (), block2);
		store.unchecked_put (transaction, block3.hash (), block3);
		store.unchecked_put (transaction, block4.hash (), block4);
		store.unchecked_put (transaction, block5.hash (), block5);
	}
	paper::pull_synchronization sync ([&blocks, &store] (paper::block const & block_a)
									{
										paper::transaction transaction (store.environment, nullptr, true);
										store.block_put (transaction, block_a.hash (), block_a);
										blocks.push_back (block_a.clone ());
									}, store);
	ASSERT_FALSE (sync.synchronize (block5.hash ()));
	ASSERT_EQ (4, blocks.size ());
	ASSERT_EQ (block2, *blocks [0]);
	ASSERT_EQ (block3, *blocks [1]);
	ASSERT_EQ (block4, *blocks [2]);
	ASSERT_EQ (block5, *blocks [3]);
}

TEST (pull_synchronization, ladder_dependencies)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::open_block block1 (0, 1, 2, 3, 4, 5);
	paper::send_block block2 (block1.hash (), 0, 1, 2, 3, 4);
	paper::open_block block3 (block2.hash (), 1, 1, 3, 4, 5);
	paper::send_block block4 (block3.hash (), 0, 1, 2, 3, 4);
	paper::receive_block block5 (block2.hash (), block4.hash (), 0, 0, 0);
	paper::send_block block6 (block5.hash (), 0, 1, 2, 3, 4);
	paper::receive_block block7 (block4.hash (), block6.hash (), 0, 0, 0);
	std::vector <std::unique_ptr <paper::block>> blocks;
	{
		paper::transaction transaction (store.environment, nullptr, true);
		store.block_put (transaction, block1.hash (), block1);
		store.unchecked_put (transaction, block2.hash (), block2);
		store.unchecked_put (transaction, block3.hash (), block3);
		store.unchecked_put (transaction, block4.hash (), block4);
		store.unchecked_put (transaction, block5.hash (), block5);
		store.unchecked_put (transaction, block6.hash (), block6);
		store.unchecked_put (transaction, block7.hash (), block7);
	}
	paper::pull_synchronization sync ([&blocks, &store] (paper::block const & block_a)
									{
										paper::transaction transaction (store.environment, nullptr, true);
										store.block_put (transaction, block_a.hash (), block_a);
										blocks.push_back (block_a.clone ());
									}, store);
	ASSERT_FALSE (sync.synchronize (block7.hash ()));
	ASSERT_EQ (6, blocks.size ());
	ASSERT_EQ (block2, *blocks [0]);
	ASSERT_EQ (block3, *blocks [1]);
	ASSERT_EQ (block4, *blocks [2]);
	ASSERT_EQ (block5, *blocks [3]);
	ASSERT_EQ (block6, *blocks [4]);
	ASSERT_EQ (block7, *blocks [5]);
}

TEST (push_synchronization, empty)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	std::vector <std::unique_ptr <paper::block>> blocks;
	paper::push_synchronization sync ([&blocks] (paper::block const & block_a)
	{
		blocks.push_back (block_a.clone ());
	}, store);
	ASSERT_TRUE (sync.synchronize (0));
	ASSERT_EQ (0, blocks.size ());
}

TEST (push_synchronization, one)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::open_block block1 (0, 1, 2, 3, 4, 5);
	paper::send_block block2 (block1.hash (), 0, 1, 2, 3, 4);
	std::vector <std::unique_ptr <paper::block>> blocks;
	{
		paper::transaction transaction (store.environment, nullptr, true);
		store.block_put (transaction, block1.hash (), block1);
		store.block_put (transaction, block2.hash (), block2);
	}
	paper::push_synchronization sync ([&blocks, &store] (paper::block const & block_a)
									{
										paper::transaction transaction (store.environment, nullptr, true);
										store.block_put (transaction, block_a.hash (), block_a);
										blocks.push_back (block_a.clone ());
									}, store);
	{
		paper::transaction transaction (store.environment, nullptr, true);
		store.unsynced_put (transaction, block2.hash ());
	}
	ASSERT_FALSE (sync.synchronize (block2.hash ()));
	ASSERT_EQ (1, blocks.size ());
	ASSERT_EQ (block2, *blocks [0]);
}