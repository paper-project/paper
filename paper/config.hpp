#pragma once

#include <cstddef>

namespace paper
{
// Network variants with different genesis blocks and network parameters
enum class paper_networks
{
	// Low work parameters, publicly known genesis key, test IP ports
	paper_test_network,
	// Normal work parameters, secret beta genesis key, beta IP ports
	paper_beta_network,
	// Normal work parameters, secret live key, live IP ports
	paper_live_network
};
paper::paper_networks const paper_network = paper_networks::ACTIVE_NETWORK;
int const database_check_interval = paper_network == paper::paper_networks::paper_test_network ? 4 : 1024;
size_t const database_size_increment = paper_network == paper::paper_networks::paper_test_network ? 2 * 1024 * 1024 : 256 * 1024 * 1024;
}