#include <gtest/gtest.h>
#include <paper/node.hpp>

#include <fstream>

TEST (landing, serialization)
{
	paper::landing_store store1 (0, 1, 2, 3);
	auto file (paper::unique_path ());
	{
		std::ofstream stream;
		stream.open (file.string ());
		ASSERT_FALSE (stream.fail ());
		store1.serialize (stream);
	}
	std::ifstream stream;
	stream.open (file.string ());
	ASSERT_FALSE (stream.fail ());
	bool error;
	paper::landing_store store2 (error, stream);
	ASSERT_FALSE (error);
	ASSERT_EQ (store1, store2);
}

TEST (landing, overwrite)
{
	paper::landing_store store1 (0, 1, 2, 3);
	auto file (paper::unique_path ());
	for (auto i (0); i < 10; ++i)
	{
		store1.last += i;
		{
			std::ofstream stream;
			stream.open (file.string ());
			ASSERT_FALSE (stream.fail ());
			store1.serialize (stream);
		}
		{
			std::ifstream stream;
			stream.open (file.string ());
			ASSERT_FALSE (stream.fail ());
			bool error;
			paper::landing_store store2 (error, stream);
			ASSERT_FALSE (error);
			ASSERT_EQ (store1, store2);
		}
	}
}

TEST (landing, start)
{
	paper::system system (24000, 1);
	paper::keypair key;
	auto path (paper::unique_path ());
	paper::landing_store store (paper::test_genesis_key.pub, key.pub, std::numeric_limits <uint64_t>::max (), std::numeric_limits <uint64_t>::max ());
	paper::landing landing (*system.nodes [0], system.wallet(0), store, path);
}