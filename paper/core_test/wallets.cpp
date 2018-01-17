#include <gtest/gtest.h>

#include <paper/node.hpp>

TEST (wallets, DISABLED_open_create)
{
	paper::system system (24000, 1);
	bool error (false);
	paper::wallets wallets (error, *system.nodes [0]);
	ASSERT_FALSE (error);
	ASSERT_EQ (0, wallets.items.size ());
	paper::uint256_union id;
	ASSERT_EQ (nullptr, wallets.open (id));
	auto wallet (wallets.create (id));
	ASSERT_NE (nullptr, wallet);
	ASSERT_EQ (wallet, wallets.open (id));
}

TEST (wallets, DISABLED_open_existing)
{
	paper::system system (24000, 1);
	paper::uint256_union id;
	{
		bool error (false);
		paper::wallets wallets (error, *system.nodes [0]);
		ASSERT_FALSE (error);
		ASSERT_EQ (0, wallets.items.size ());
		auto wallet (wallets.create (id));
		ASSERT_NE (nullptr, wallet);
		ASSERT_EQ (wallet, wallets.open (id));
		auto iterations (0);
		while (wallet->store.password.value () == 0)
		{
			system.poll ();
			++iterations;
			ASSERT_LT (iterations, 200);
		}
	}
	{
		bool error (false);
		paper::wallets wallets (error, *system.nodes [0]);
		ASSERT_FALSE (error);
		ASSERT_EQ (1, wallets.items.size ());
		ASSERT_NE (nullptr, wallets.open (id));
	}
}

TEST (wallets, DISABLED_remove)
{
	paper::system system (24000, 1);
	paper::uint256_union one (1);
	{
		bool error (false);
		paper::wallets wallets (error, *system.nodes [0]);
		ASSERT_FALSE (error);
		ASSERT_EQ (0, wallets.items.size ());
		auto wallet (wallets.create (one));
		ASSERT_NE (nullptr, wallet);
		ASSERT_EQ (1, wallets.items.size ());
		wallets.destroy (one);
		ASSERT_EQ (0, wallets.items.size ());
	}
	{
		bool error (false);
		paper::wallets wallets (error, *system.nodes [0]);
		ASSERT_FALSE (error);
		ASSERT_EQ (0, wallets.items.size ());
	}
}