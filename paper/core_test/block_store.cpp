#include <gtest/gtest.h>
#include <paper/node.hpp>

#include <fstream>

TEST (block_store, construction)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
    ASSERT_TRUE (!init);
    auto now (store.now ());
    ASSERT_GT (now, 1408074640);
}

TEST (block_store, add_item)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
    ASSERT_TRUE (!init);
    paper::open_block block (0, 1, 0, 0, 0, 0);
    paper::uint256_union hash1 (block.hash ());
	paper::transaction transaction (store.environment, nullptr, true);
    auto latest1 (store.block_get (transaction, hash1));
    ASSERT_EQ (nullptr, latest1);
    ASSERT_FALSE (store.block_exists (transaction, hash1));
    store.block_put (transaction, hash1, block);
    auto latest2 (store.block_get (transaction, hash1));
    ASSERT_NE (nullptr, latest2);
    ASSERT_EQ (block, *latest2);
    ASSERT_TRUE (store.block_exists (transaction, hash1));
	ASSERT_FALSE (store.block_exists (transaction, hash1.number () - 1));
	store.block_del (transaction, hash1);
	auto latest3 (store.block_get (transaction, hash1));
	ASSERT_EQ (nullptr, latest3);
}

TEST (block_store, add_nonempty_block)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
    ASSERT_TRUE (!init);
    paper::keypair key1;
    paper::open_block block (0, 1, 0, 0, 0, 0);
    paper::uint256_union hash1 (block.hash ());
    block.signature = paper::sign_message (key1.prv, key1.pub, hash1);
	paper::transaction transaction (store.environment, nullptr, true);
    auto latest1 (store.block_get (transaction, hash1));
    ASSERT_EQ (nullptr, latest1);
    store.block_put (transaction, hash1, block);
    auto latest2 (store.block_get (transaction, hash1));
    ASSERT_NE (nullptr, latest2);
    ASSERT_EQ (block, *latest2);
}

TEST (block_store, add_two_items)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
    ASSERT_TRUE (!init);
    paper::keypair key1;
    paper::open_block block (0, 1, 1, 0, 0, 0);
    paper::uint256_union hash1 (block.hash ());
    block.signature = paper::sign_message (key1.prv, key1.pub, hash1);
	paper::transaction transaction (store.environment, nullptr, true);
    auto latest1 (store.block_get (transaction, hash1));
    ASSERT_EQ (nullptr, latest1);
    paper::open_block block2 (0, 1, 3, 0, 0, 0);
    block2.hashables.account = 3;
    paper::uint256_union hash2 (block2.hash ());
    block2.signature = paper::sign_message (key1.prv, key1.pub, hash2);
    auto latest2 (store.block_get (transaction, hash2));
    ASSERT_EQ (nullptr, latest2);
    store.block_put (transaction, hash1, block);
    store.block_put (transaction, hash2, block2);
    auto latest3 (store.block_get (transaction, hash1));
    ASSERT_NE (nullptr, latest3);
    ASSERT_EQ (block, *latest3);
    auto latest4 (store.block_get (transaction, hash2));
    ASSERT_NE (nullptr, latest4);
    ASSERT_EQ (block2, *latest4);
    ASSERT_FALSE (*latest3 == *latest4);
}

TEST (block_store, add_receive)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
    ASSERT_TRUE (!init);
    paper::keypair key1;
    paper::keypair key2;
	paper::open_block block1 (0, 1, 0, 0, 0, 0);
	paper::transaction transaction (store.environment, nullptr, true);
	store.block_put (transaction, block1.hash (), block1);
    paper::receive_block block (block1.hash (), 1, 1, 2, 3);
    paper::block_hash hash1 (block.hash ());
    auto latest1 (store.block_get (transaction, hash1));
    ASSERT_EQ (nullptr, latest1);
    store.block_put (transaction, hash1, block);
    auto latest2 (store.block_get (transaction, hash1));
    ASSERT_NE (nullptr, latest2);
    ASSERT_EQ (block, *latest2);
}

TEST (block_store, add_pending)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
    ASSERT_TRUE (!init);
    paper::keypair key1;
    paper::block_hash hash1 (0);
    paper::receivable receivable1;
	paper::transaction transaction (store.environment, nullptr, true);
    ASSERT_TRUE (store.pending_get (transaction, hash1, receivable1));
    store.pending_put (transaction, hash1, receivable1);
    paper::receivable receivable2;
    ASSERT_FALSE (store.pending_get (transaction, hash1, receivable2));
    ASSERT_EQ (receivable1, receivable2);
    store.pending_del (transaction, hash1);
    ASSERT_TRUE (store.pending_get (transaction, hash1, receivable2));
}

TEST (block_store, pending_iterator)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
    ASSERT_TRUE (!init);
	paper::transaction transaction (store.environment, nullptr, true);
    ASSERT_EQ (store.pending_end (), store.pending_begin (transaction));
    store.pending_put (transaction, 1, {2, 3, 4});
    auto current (store.pending_begin (transaction));
    ASSERT_NE (store.pending_end (), current);
    ASSERT_EQ (paper::account (1), current->first);
	paper::receivable receivable (current->second);
    ASSERT_EQ (paper::account (2), receivable.source);
    ASSERT_EQ (paper::amount (3), receivable.amount);
    ASSERT_EQ (paper::account (4), receivable.destination);
}

TEST (block_store, genesis)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
    ASSERT_TRUE (!init);
    paper::genesis genesis;
    auto hash (genesis.hash ());
	paper::transaction transaction (store.environment, nullptr, true);
    genesis.initialize (transaction, store);
    paper::account_info info;
    ASSERT_FALSE (store.account_get (transaction, paper::genesis_account, info));
	ASSERT_EQ (hash, info.head);
    auto block1 (store.block_get (transaction, info.head));
    ASSERT_NE (nullptr, block1);
    auto receive1 (dynamic_cast <paper::open_block *> (block1.get ()));
    ASSERT_NE (nullptr, receive1);
    ASSERT_LE (info.modified, store.now ());
	auto test_pub_text (paper::test_genesis_key.pub.to_string ());
	auto test_pub_account (paper::test_genesis_key.pub.to_base58check ());
	auto test_prv_text (paper::test_genesis_key.prv.to_string ());
	ASSERT_EQ (paper::genesis_account, paper::test_genesis_key.pub);
}

TEST (representation, changes)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
    ASSERT_TRUE (!init);
    paper::keypair key1;
	paper::transaction transaction (store.environment, nullptr, true);
    ASSERT_EQ (0, store.representation_get (transaction, key1.pub));
    store.representation_put (transaction, key1.pub, 1);
    ASSERT_EQ (1, store.representation_get (transaction, key1.pub));
    store.representation_put (transaction, key1.pub, 2);
    ASSERT_EQ (2, store.representation_get (transaction, key1.pub));
}

TEST (bootstrap, simple)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
    ASSERT_TRUE (!init);
    paper::send_block block1 (0, 1, 2, 3, 4, 5);
	paper::transaction transaction (store.environment, nullptr, true);
    auto block2 (store.unchecked_get (transaction, block1.previous ()));
    ASSERT_EQ (nullptr, block2);
    store.unchecked_put (transaction, block1.previous (), block1);
    auto block3 (store.unchecked_get (transaction, block1.previous ()));
    ASSERT_NE (nullptr, block3);
    ASSERT_EQ (block1, *block3);
    store.unchecked_del (transaction, block1.previous ());
    auto block4 (store.unchecked_get (transaction, block1.previous ()));
    ASSERT_EQ (nullptr, block4);
}

TEST (checksum, simple)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
    ASSERT_TRUE (!init);
	paper::block_hash hash0 (0);
	paper::transaction transaction (store.environment, nullptr, true);
    ASSERT_TRUE (store.checksum_get (transaction, 0x100, 0x10, hash0));
    paper::block_hash hash1 (0);
    store.checksum_put (transaction, 0x100, 0x10, hash1);
    paper::block_hash hash2;
    ASSERT_FALSE (store.checksum_get (transaction, 0x100, 0x10, hash2));
    ASSERT_EQ (hash1, hash2);
    store.checksum_del (transaction, 0x100, 0x10);
    paper::block_hash hash3;
    ASSERT_TRUE (store.checksum_get (transaction, 0x100, 0x10, hash3));
}

TEST (block_store, empty_accounts)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
    ASSERT_TRUE (!init);
	paper::transaction transaction (store.environment, nullptr, false);
    auto begin (store.latest_begin (transaction));
    auto end (store.latest_end ());
    ASSERT_EQ (end, begin);
}

TEST (block_store, one_block)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
    ASSERT_TRUE (!init);
    paper::open_block block1 (0, 1, 0, 0, 0, 0);
	paper::transaction transaction (store.environment, nullptr, true);
    store.block_put (transaction, block1.hash (), block1);
	ASSERT_TRUE (store.block_exists (transaction, block1.hash ()));
}

TEST (block_store, empty_bootstrap)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
    ASSERT_TRUE (!init);
	paper::transaction transaction (store.environment, nullptr, false);
    auto begin (store.unchecked_begin (transaction));
    auto end (store.unchecked_end ());
    ASSERT_EQ (end, begin);
}

TEST (block_store, one_bootstrap)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
    ASSERT_TRUE (!init);
    paper::send_block block1 (0, 1, 2, 3, 4, 5);
	paper::transaction transaction (store.environment, nullptr, true);
    store.unchecked_put (transaction, block1.hash (), block1);
    auto begin (store.unchecked_begin (transaction));
    auto end (store.unchecked_end ());
    ASSERT_NE (end, begin);
    auto hash1 (begin->first);
    ASSERT_EQ (block1.hash (), hash1);
    auto block2 (paper::deserialize_block (begin->second));
    ASSERT_EQ (block1, *block2);
    ++begin;
    ASSERT_EQ (end, begin);
}

TEST (block_store, frontier_retrieval)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
    ASSERT_TRUE (!init);
    paper::account account1 (0);
    paper::account_info info1 (0, 0, 0, 0, false);
	paper::transaction transaction (store.environment, nullptr, true);
    store.account_put (transaction, account1, info1);
    paper::account_info info2;
    store.account_get (transaction, account1, info2);
    ASSERT_EQ (info1, info2);
}

TEST (block_store, one_account)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
    ASSERT_TRUE (!init);
    paper::account account (0);
    paper::block_hash hash (0);
	paper::transaction transaction (store.environment, nullptr, true);
    store.account_put (transaction, account, {hash, account, 42, 100, false});
    auto begin (store.latest_begin (transaction));
    auto end (store.latest_end ());
    ASSERT_NE (end, begin);
    ASSERT_EQ (account, begin->first);
	paper::account_info info (begin->second);
    ASSERT_EQ (hash, info.head);
    ASSERT_EQ (42, info.balance.number ());
    ASSERT_EQ (100, info.modified);
    ++begin;
    ASSERT_EQ (end, begin);
}

TEST (block_store, two_block)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
    ASSERT_TRUE (!init);
    paper::open_block block1 (0, 1, 1, 0, 0, 0);
    block1.hashables.account = 1;
    std::vector <paper::block_hash> hashes;
    std::vector <paper::open_block> blocks;
    hashes.push_back (block1.hash ());
    blocks.push_back (block1);
	paper::transaction transaction (store.environment, nullptr, true);
    store.block_put (transaction, hashes [0], block1);
    paper::open_block block2 (0, 1, 2, 0, 0, 0);
    hashes.push_back (block2.hash ());
    blocks.push_back (block2);
    store.block_put (transaction, hashes [1], block2);
	ASSERT_TRUE (store.block_exists (transaction, block1.hash ()));
	ASSERT_TRUE (store.block_exists (transaction, block2.hash ()));
}

TEST (block_store, two_account)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
    ASSERT_TRUE (!init);
    paper::account account1 (1);
    paper::block_hash hash1 (2);
    paper::account account2 (3);
    paper::block_hash hash2 (4);
	paper::transaction transaction (store.environment, nullptr, true);
    store.account_put (transaction, account1, {hash1, account1, 42, 100, false});
    store.account_put (transaction, account2, {hash2, account2, 84, 200, false});
    auto begin (store.latest_begin (transaction));
    auto end (store.latest_end ());
    ASSERT_NE (end, begin);
    ASSERT_EQ (account1, begin->first);
	paper::account_info info1 (begin->second);
    ASSERT_EQ (hash1, info1.head);
    ASSERT_EQ (42, info1.balance.number ());
    ASSERT_EQ (100, info1.modified);
    ++begin;
    ASSERT_NE (end, begin);
    ASSERT_EQ (account2, begin->first);
	paper::account_info info2 (begin->second);
    ASSERT_EQ (hash2, info2.head);
    ASSERT_EQ (84, info2.balance.number ());
    ASSERT_EQ (200, info2.modified);
    ++begin;
    ASSERT_EQ (end, begin);
}

TEST (block_store, latest_find)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
    ASSERT_TRUE (!init);
    paper::account account1 (1);
    paper::block_hash hash1 (2);
    paper::account account2 (3);
    paper::block_hash hash2 (4);
	paper::transaction transaction (store.environment, nullptr, true);
    store.account_put (transaction, account1, {hash1, account1, 100, 0, false});
    store.account_put (transaction, account2, {hash2, account2, 200, 0, false});
    auto first (store.latest_begin (transaction));
    auto second (store.latest_begin (transaction));
    ++second;
    auto find1 (store.latest_begin (transaction, 1));
    ASSERT_EQ (first, find1);
    auto find2 (store.latest_begin (transaction, 3));
    ASSERT_EQ (second, find2);
    auto find3 (store.latest_begin (transaction, 2));
    ASSERT_EQ (second, find3);
}

TEST (block_store, bad_path)
{
    bool init (false);
    paper::block_store store (init, boost::filesystem::path ("///"));
    ASSERT_TRUE (init);
}

TEST (block_store, DISABLED_already_open) // File can be shared
{
    auto path (paper::unique_path ());
    boost::filesystem::create_directories (path.parent_path ());
    std::ofstream file;
    file.open (path.string ().c_str ());
    ASSERT_TRUE (file.is_open ());
    bool init (false);
    paper::block_store store (init, path);
    ASSERT_TRUE (init);
}

TEST (block_store, roots)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	paper::send_block send_block (0, 1, 2, 3, 4, 5);
	ASSERT_EQ (send_block.hashables.previous, send_block.root ());
	paper::change_block change_block (0, 1, 2, 3, 4);
	ASSERT_EQ (change_block.hashables.previous, change_block.root ());
	paper::receive_block receive_block (0, 1, 2, 3, 4);
	ASSERT_EQ (receive_block.hashables.previous, receive_block.root ());
	paper::open_block open_block (0, 1, 2, 3, 4, 5);
	ASSERT_EQ (open_block.hashables.account, open_block.root ());
}

TEST (block_store, pending_exists)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
    paper::block_hash two (2);
    paper::receivable receivable;
	paper::transaction transaction (store.environment, nullptr, true);
    store.pending_put (transaction, two, receivable);
    paper::block_hash one (1);
    ASSERT_FALSE (store.pending_exists (transaction, one));
}

TEST (block_store, latest_exists)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
    paper::block_hash two (2);
    paper::account_info info;
	paper::transaction transaction (store.environment, nullptr, true);
    store.account_put (transaction, two, info);
    paper::block_hash one (1);
    ASSERT_FALSE (store.account_exists (one));
}

TEST (block_store, stack)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
    paper::block_hash hash1 (1);
    store.stack_push (0, hash1);
    paper::block_hash hash2 (2);
    store.stack_push (1, hash2);
    auto hash3 (store.stack_pop (1));
    ASSERT_EQ (hash2, hash3);
    auto hash4 (store.stack_pop (0));
    ASSERT_EQ (hash1, hash4);
}

TEST (block_store, unsynced)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	paper::transaction transaction (store.environment, nullptr, true);
    ASSERT_EQ (store.unsynced_end (), store.unsynced_begin (transaction));
    paper::block_hash hash1 (0);
    ASSERT_FALSE (store.unsynced_exists (transaction, hash1));
    store.unsynced_put (transaction, hash1);
    ASSERT_TRUE (store.unsynced_exists (transaction, hash1));
    ASSERT_NE (store.unsynced_end (), store.unsynced_begin (transaction));
    ASSERT_EQ (hash1, paper::uint256_union (store.unsynced_begin (transaction)->first));
    store.unsynced_del (transaction, hash1);
    ASSERT_FALSE (store.unsynced_exists (transaction, hash1));
    ASSERT_EQ (store.unsynced_end (), store.unsynced_begin (transaction));
}

TEST (block_store, unsynced_iteration)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	paper::transaction transaction (store.environment, nullptr, true);
    ASSERT_EQ (store.unsynced_end (), store.unsynced_begin (transaction));
    paper::block_hash hash1 (1);
    store.unsynced_put (transaction, hash1);
    paper::block_hash hash2 (2);
    store.unsynced_put (transaction, hash2);
    std::unordered_set <paper::block_hash> hashes;
    for (auto i (store.unsynced_begin (transaction)), n (store.unsynced_end ()); i != n; ++i)
    {
        hashes.insert (paper::uint256_union (i->first));
    }
    ASSERT_EQ (2, hashes.size ());
    ASSERT_TRUE (hashes.find (hash1) != hashes.end ());
    ASSERT_TRUE (hashes.find (hash2) != hashes.end ());
}

TEST (block_store, large_iteration)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	std::unordered_set <paper::account> accounts1;
	for (auto i (0); i < 10000; ++i)
	{
		paper::transaction transaction (store.environment, nullptr, true);
		paper::account account;
		paper::random_pool.GenerateBlock (account.bytes.data (), account.bytes.size ());
		accounts1.insert (account);
		store.account_put (transaction, account, paper::account_info ());
	}
	std::unordered_set <paper::account> accounts2;
	paper::account previous (0);
	paper::transaction transaction (store.environment, nullptr, false);
	for (auto i (store.latest_begin (transaction, 0)), n (store.latest_end ()); i != n; ++i)
	{
		paper::account current (i->first);
		assert (current.number () > previous.number ());
		accounts2.insert (current);
		previous = current;
	}
	ASSERT_EQ (accounts1, accounts2);
}

TEST (block_store, frontier)
{
    bool init (false);
    paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	paper::transaction transaction (store.environment, nullptr, true);
	paper::block_hash hash (100);
	paper::account account (200);
	ASSERT_TRUE (store.frontier_get (transaction, hash).is_zero ());
	store.frontier_put (transaction, hash, account);
	ASSERT_EQ (account, store.frontier_get (transaction, hash));
	store.frontier_del (transaction, hash);
	ASSERT_TRUE (store.frontier_get (transaction, hash).is_zero ());
}