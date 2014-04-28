#include "MessageDirector.h"
#include "MDNetworkParticipant.h"
#include "MDNetworkUpstream.h"
#include "core/global.h"
#include "config/ConfigVariable.h"
#include "config/constraints.h"
#include <algorithm>
#include <functional>
#include <boost/icl/interval_bounds.hpp>
using boost::asio::ip::tcp;

static ConfigGroup md_config("messagedirector");
static ConfigVariable<std::string> bind_addr("bind", "unspecified", md_config);
static ConfigVariable<std::string> connect_addr("connect", "unspecified", md_config);
static ValidAddressConstraint valid_bind_addr(bind_addr);
static ValidAddressConstraint valid_connect_addr(connect_addr);

static ConfigGroup daemon_config("daemon");
static ConfigVariable<std::string> daemon_name("name", "<unnamed>", daemon_config);
static ConfigVariable<std::string> daemon_url("url", "", daemon_config);

MessageDirector MessageDirector::singleton;


MessageDirector::MessageDirector() : m_net_acceptor(NULL), m_upstream(NULL), m_initialized(false),
	m_log("msgdir", "Message Director")
{
}

MessageDirector::~MessageDirector()
{
	for(auto it = m_participants.begin(); it != m_participants.end(); ++it) {
		delete *it;
	}
	m_participants.clear();
}

void MessageDirector::init_network()
{
	if(!m_initialized)
	{
		// Bind to port and listen for downstream servers
		if(bind_addr.get_val() != "unspecified")
		{
			m_log.info() << "Opening listening socket..." << std::endl;

			AcceptorCallback callback = std::bind(&MessageDirector::handle_connection,
			                                      this, std::placeholders::_1);
			m_net_acceptor = new NetworkAcceptor(io_service, callback);
			boost::system::error_code ec;
			ec = m_net_acceptor->bind(bind_addr.get_val(), 7199);
			if(ec.value() != 0)
			{
				m_log.fatal() << "Could not bind listening port: "
				              << bind_addr.get_val() << std::endl;
				m_log.fatal() << "Error code: " << ec.value()
				              << "(" << ec.category().message(ec.value()) << ")"
				              << std::endl;
				exit(1);
			}
			m_net_acceptor->start();
		}

		// Connect to upstream server and start handling received messages
		if(connect_addr.get_val() != "unspecified")
		{
			m_log.info() << "Connecting upstream..." << std::endl;

			MDNetworkUpstream *upstream = new MDNetworkUpstream(this);

			boost::system::error_code ec;
			upstream->connect(connect_addr.get_val());
			if(ec.value() != 0)
			{
				m_log.fatal() << "Could not connect to remote MD at IP: "
				              << connect_addr.get_val() << std::endl;
				m_log.fatal() << "Error code: " << ec.value()
				              << "(" << ec.category().message(ec.value()) << ")"
				              << std::endl;
				exit(1);
			}

			m_upstream = upstream;
		}
	}
}

void MessageDirector::route_datagram(MDParticipantInterface *p, DatagramHandle dg)
{
	m_log.trace() << "Processing datagram...." << std::endl;

	std::list<channel_t> channels;
	DatagramIterator dgi(dg);
	std::set<ChannelSubscriber*> receiving_participants;
	try
	{
		uint8_t channel_count = dgi.read_uint8();

		// Route messages to participants
		auto &receive_log = m_log.trace();
		receive_log << "Receivers: ";
		for(uint8_t i = 0; i < channel_count; ++i)
		{
			channel_t channel = dgi.read_channel();
			receive_log << channel << ", ";
			channels.push_back(channel);
		}
		receive_log << std::endl;
	}
	catch(DatagramIteratorEOF &)
	{
		if(p)
		{
			// Log error with receivers output
			m_log.error() << "Detected truncated datagram reading header from '"
			              << p->m_name << "'.\n";
		}
		else
		{
			// Log error with receivers output
			m_log.error() << "Detected truncated datagram reading header from unknown participant.\n";
		}
		return;
	}

	lookup_channels(channels, receiving_participants);

	if(p)
	{
		receiving_participants.erase(p);
	}

	for(auto it = receiving_participants.begin(); it != receiving_participants.end(); ++it)
	{
		auto participant = static_cast<MDParticipantInterface*>(*it);
		DatagramIterator msg_dgi(dg, dgi.tell());
		try
		{
			participant->handle_datagram(dg, msg_dgi);
		}
		catch(DatagramIteratorEOF &)
		{
			// Log error with receivers output
			m_log.error() << "Detected truncated datagram in handle_datagram for '" << participant->m_name << "'"
			              " from participant '" << p->m_name << "'." << std::endl;
			return;
		}
	}

	if(p && m_upstream)  // Send message upstream
	{
		m_upstream->handle_datagram(dg);
		m_log.trace() << "...routing upstream." << std::endl;
	}
	else if(!p) // If there is no participant, then it came from the upstream
	{
		m_log.trace() << "...not routing upstream: It came from there." << std::endl;
	}
	else // Otherwise is root node
	{
		m_log.trace() << "...not routing upstream: There is none." << std::endl;
	}
}

void MessageDirector::on_add_channel(channel_t c)
{
	if(m_upstream)
	{
		// Send upstream control message
		m_upstream->subscribe_channel(c);
	}
}

void MessageDirector::on_remove_channel(channel_t c)
{
	if(m_upstream)
	{
		// Send upstream control message
		m_upstream->unsubscribe_channel(c);
	}
}

void MessageDirector::on_add_range(channel_t lo, channel_t hi)
{
	if(m_upstream)
	{
		// Send upstream control message
		m_upstream->subscribe_range(lo, hi);
	}
}

void MessageDirector::on_remove_range(channel_t lo, channel_t hi)
{
	if(m_upstream)
	{
		// Send upstream control message
		m_upstream->unsubscribe_range(lo, hi);
	}
}

void MessageDirector::handle_connection(tcp::socket *socket)
{
	boost::asio::ip::tcp::endpoint remote;
	remote = socket->remote_endpoint();
	m_log.info() << "Got an incoming connection from "
	             << remote.address() << ":" << remote.port() << std::endl;
	new MDNetworkParticipant(socket); // It deletes itself when connection is lost
}

void MessageDirector::add_participant(MDParticipantInterface* p)
{
	m_participants.insert(m_participants.end(), p);
}

void MessageDirector::remove_participant(MDParticipantInterface* p)
{
	unsubscribe_all(p);

	// Stop tracking participant
	m_participants.remove(p);

	// Send out any post-remove messages the participant may have added.
	// N.B. this is done last, because we don't want to send messages
	// through the Director while a participant is being removed, as
	// certain data structures may not have their invariants satisfied
	// during that time.
	p->post_remove();
}

void MessageDirector::receive_datagram(DatagramHandle dg)
{
	route_datagram(NULL, dg);
}

void MessageDirector::receive_disconnect()
{
	m_log.fatal() << "Lost connection to upstream md" << std::endl;
	exit(1);
}
