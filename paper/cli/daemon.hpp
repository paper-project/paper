#include <paper/node.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/asio/ip/address_v6.hpp>

namespace paper_daemon
{
    class daemon
    {
    public:
        void run ();
    };
    class daemon_config
    {
    public:
        daemon_config ();
        daemon_config (bool &, std::istream &);
        void serialize (std::ostream &);
		bool rpc_enable;
		paper::rpc_config rpc;
		paper::node_config node;
    };
}