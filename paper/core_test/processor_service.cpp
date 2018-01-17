#include <gtest/gtest.h>
#include <paper/node.hpp>

#include <thread>
#include <atomic>
#include <condition_variable>

TEST (processor_service, bad_send_signature)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::account_info info1;
	ASSERT_FALSE (store.account_get (transaction, paper::test_genesis_key.pub, info1));
	paper::keypair key2;
	paper::send_block send (info1.head, paper::test_genesis_key.pub, 50, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	paper::block_hash hash1 (send.hash ());
	send.signature.bytes [32] ^= 0x1;
	ASSERT_EQ (paper::process_result::bad_signature, ledger.process (transaction, send).code);
}

TEST (processor_service, bad_receive_signature)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::account_info info1;
	ASSERT_FALSE (store.account_get (transaction, paper::test_genesis_key.pub, info1));
	paper::keypair key2;
	paper::send_block send (info1.head, key2.pub, 50, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	paper::block_hash hash1 (send.hash ());
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, send).code);
	paper::account_info info2;
	ASSERT_FALSE (store.account_get (transaction, paper::test_genesis_key.pub, info2));
	paper::receive_block receive (key2.pub, hash1, key2.prv, key2.pub, 0);
	receive.signature.bytes [32] ^= 0x1;
	ASSERT_EQ (paper::process_result::bad_signature, ledger.process (transaction, receive).code);
}

TEST (processor_service, empty)
{
	paper::processor_service service;
	std::thread thread ([&service] () {service.run ();});
	service.stop ();
	thread.join ();
}

TEST (processor_service, one)
{
	paper::processor_service service;
	std::atomic <bool> done (false);
	std::mutex mutex;
	std::condition_variable condition;
	service.add (std::chrono::system_clock::now (), [&] ()
				 {
					 std::lock_guard <std::mutex> lock (mutex);
					 done = true;
					 condition.notify_one ();
				 });
	std::thread thread ([&service] () {service.run ();});
	std::unique_lock <std::mutex> unique (mutex);
	condition.wait (unique, [&] () {return !!done;});
	service.stop ();
	thread.join ();
}

TEST (processor_service, many)
{
	paper::processor_service service;
	std::atomic <int> count (0);
	std::mutex mutex;
	std::condition_variable condition;
	for (auto i (0); i < 50; ++i)
	{
		service.add (std::chrono::system_clock::now (), [&] ()
					 {
						 std::lock_guard <std::mutex> lock (mutex);
						 count += 1;
						 condition.notify_one ();
					 });
	}
	std::vector <std::thread> threads;
	for (auto i (0); i < 50; ++i)
	{
		threads.push_back (std::thread ([&service] () {service.run ();}));
	}
	std::unique_lock <std::mutex> unique (mutex);
	condition.wait (unique, [&] () {return count == 50;});
	service.stop ();
	for (auto i (threads.begin ()), j (threads.end ()); i != j; ++i)
	{
		i->join ();
	}
}

TEST (processor_service, top_execution)
{
	paper::processor_service service;
	int value (0);
	std::mutex mutex;
	std::unique_lock <std::mutex> lock1 (mutex);
	service.add (std::chrono::system_clock::now (), [&] () {value = 1; service.stop (); lock1.unlock ();});
	service.add (std::chrono::system_clock::now () + std::chrono::milliseconds (1), [&] () {value = 2; service.stop (); lock1.unlock ();});
	service.run ();
	std::unique_lock <std::mutex> lock2 (mutex);
	ASSERT_EQ (1, value);
}

TEST (processor_service, stopping)
{
	paper::processor_service service;
	ASSERT_EQ (0, service.operations.size ());
	service.add (std::chrono::system_clock::now (), [] () {});
	ASSERT_EQ (1, service.operations.size ());
	service.stop ();
	ASSERT_EQ (0, service.operations.size ());
	service.add (std::chrono::system_clock::now (), [] () {});
	ASSERT_EQ (0, service.operations.size ());
}