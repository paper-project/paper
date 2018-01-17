#include <gtest/gtest.h>
#include <paper/node.hpp>

#include <thread>

TEST (system, generate_mass_activity)
{
    paper::system system (24000, 1);
    system.wallet (0)->insert (paper::test_genesis_key.prv);
    size_t count (20);
    system.generate_mass_activity (count, *system.nodes [0]);
    size_t accounts (0);
	paper::transaction transaction (system.nodes [0]->store.environment, nullptr, false);
    for (auto i (system.nodes [0]->store.latest_begin (transaction)), n (system.nodes [0]->store.latest_end ()); i != n; ++i)
    {
        ++accounts;
    }
    ASSERT_GT (accounts, count / 10);
}

TEST (system, generate_mass_activity_long)
{
	std::vector <std::thread> threads;
	{
		paper::system system (24000, 1);
		for (auto i (0), n (4); i != n; ++i)
		{
			threads.push_back (std::thread ([&system] ()
			{
				system.service->run ();
			}));
		}
		for (auto i (0), n (4); i != n; ++i)
		{
			threads.push_back (std::thread ([&system] ()
			{
				system.processor.run ();
			}));
		}
		system.wallet (0)->insert (paper::test_genesis_key.prv);
		size_t count (10000);
		system.generate_mass_activity (count, *system.nodes [0]);
		size_t accounts (0);
		paper::transaction transaction (system.nodes [0]->store.environment, nullptr, false);
		for (auto i (system.nodes [0]->store.latest_begin (transaction)), n (system.nodes [0]->store.latest_end ()); i != n; ++i)
		{
			++accounts;
		}
		ASSERT_GT (accounts, count / 10);
	}
	for (auto i (threads.begin ()), n (threads.end ()); i != n; ++i)
	{
		i->join ();
	}
}

TEST (ledger, deep_account_compute)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key;
	auto balance (paper::genesis_amount - 1);
	paper::send_block send (genesis.hash (), key.pub, balance, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, send).code);
	paper::open_block open (send.hash (), paper::test_genesis_key.pub, key.pub, key.prv, key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, open).code);
	auto sprevious (send.hash ());
	auto rprevious (open.hash ());
	for (auto i (0), n (100000); i != n; ++i)
	{
		balance -= 1;
		paper::send_block send (sprevious, key.pub, balance, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
		ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, send).code);
		sprevious = send.hash ();
		paper::receive_block receive (rprevious, send.hash (), key.prv, key.pub, 0);
		ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, receive).code);
		rprevious = receive.hash ();
		if (i % 100 == 0)
		{
			std::cerr << i << ' ';
		}
		auto account (ledger.account (transaction, sprevious));
		auto balance (ledger.balance (transaction, rprevious));
	}
}

TEST (wallet, multithreaded_send)
{
	std::vector <std::thread> threads;
	{
		paper::system system (24000, 1);
		paper::keypair key;
		auto wallet_l (system.wallet (0));
		wallet_l->insert (paper::test_genesis_key.prv);
		for (auto i (0); i < 20; ++i)
		{
			threads.push_back (std::thread ([wallet_l, &key] ()
			{
				for (auto i (0); i < 1000; ++i)
				{
					wallet_l->send_sync (paper::test_genesis_key.pub, key.pub, 1000);
				}
			}));
		}
		while (system.nodes [0]->balance(paper::test_genesis_key.pub) != (paper::genesis_amount - 20 * 1000 * 1000))
		{
			system.poll ();
		}
	}
	for (auto i (threads.begin ()), n (threads.end ()); i != n; ++i)
	{
		i->join ();
	}
}

TEST (store, load)
{
	paper::system system (24000, 1);
	std::vector <std::thread> threads;
	for (auto i (0); i < 100; ++i)
	{
		threads.push_back (std::thread ([&system] ()
		{
			for (auto i (0); i != 1000; ++i)
			{
				paper::transaction transaction (system.nodes [0]->store.environment, nullptr, true);
				for (auto j (0); j != 10; ++j)
				{
					paper::block_hash hash;
					paper::random_pool.GenerateBlock (hash.bytes.data (), hash.bytes.size ());
					system.nodes [0]->store.account_put (transaction, hash, paper::account_info ());
				}
			}
		}));
	}
	for (auto &i: threads)
	{
		i.join ();
	}
}