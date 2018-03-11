/*
 * chat_sever.cpp -- multiperson, multichannel, chat server
 * -> main file
*/

/*	TODO
 *
 * -refrector the code into more files, do Makefile
 *
 * -tostring/format relies on newline at the end
 *
 * -safety
 *
 * -consider custom C++ wrap library for send()
 *
 * -add C++ exception handling
 *
 * -find out why when 3 ppl join some random chars are send | temp FIX by format_
 * 
 * -add support for multichannel server
 * 
 * -add more serverside utilities:
 *  	1) runtime controlls
 *  	2) logging
 *
 * -add support for private messages
 * 
 * -consider client
*/

#include <iostream>
#include <string>
#include <set>
#include <map>

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define MAX_LEN 256	// byte limit do data send

class Fd_set 
{
public:
	Fd_set() 
	{
		clr();
	}

	void
	add( int fd)
	{
		FD_SET( fd, &fd_set_);
	}

	void	
	rm( int fd)
	{
		FD_CLR( fd, &fd_set_);
	}

	bool
	has( int fd) const
	{
		if( FD_ISSET( fd, &fd_set_))
			return true;
		else
			return false;
	}

	void
	clr()
	{
		FD_ZERO( &fd_set_);
	}

	Fd_set&
	operator=( const Fd_set & other)
	{
		clr();
		fd_set_ = other.get_set();
		return *this;
	}
	
	fd_set
	get_set() const
	{
		return fd_set_;
	}

	fd_set*
	get_set_ref()
	{
		return &fd_set_;
	}

private:
	fd_set fd_set_;
};

class Channel
{
public:
	Channel(const std::string & name, const char * port) : 
		name_( name), port_( port)
	{
		init_();
	}
		
	void
	process()
	{
		read_fds_ = master_;
		
		if( select( fdmax_ + 1, read_fds_.get_set_ref(), NULL, NULL, NULL) == -1)
		{
			perror("select");
			exit(4);
		}

		for( int i = 0; i <= fdmax_; ++i)
		{
			if( read_fds_.has( i))
			{
				std::set<int>::iterator it = unregistered_users_.find( i);
				
				if( i == listener_)
				{
					handle_new_client_();		
				} else
				{
					//handle data from client
					int nbytes;
					char buf[MAX_LEN];
					
					if( (nbytes = recv( i, buf, sizeof buf, 0)) <= 0)
					{
						// got error or connection closed by client
						connection_closing_( i, nbytes);
					}
					else if( (it != unregistered_users_.end()))
					{
						// username istn't assigned
						assign_username_( it, to_string_( buf, nbytes));
						continue;
					}
					else
					{
						// we got data from client sender
						channel_broadcast_( i, to_string_( buf, nbytes) + '\n');
					}
				}
			} // END viable fd
		} // END loop over fd
	} // END process()


private:
	Fd_set master_;			// master holds both listening and reading sockets
	Fd_set read_fds_;		// read_fds holds only read-ready sck returned by select()
	int listener_, fdmax_;		// maximum fd number
	
	std::string name_;				// name of channel
	const char * port_;				// listening port assigned to channel
	std::map< int, std::string> users_;		// map of usernames & socket fd
	std::set< int> unregistered_users_;		// list of sockets without username


	void *	// get IPv4/IPV6 addres from sockadr sa
	get_in_addr_( struct sockaddr *sa)
	{
		if( sa->sa_family == AF_INET)
			return &( ( (struct sockaddr_in*) sa)->sin_addr);
		else
			return &( ( (struct sockaddr_in6*) sa)->sin6_addr);
	}

	void	// setup socket to listen on
	init_()
	{
		struct addrinfo hints, *p, *ai;
		
		memset( &hints, 0, sizeof hints);
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags =  AI_PASSIVE;

		int rv;
		if( (rv = getaddrinfo( NULL, port_, &hints, &ai)) != 0)
		{
			fprintf( stderr, "chat_server: %s\n", gai_strerror( rv));
			exit(1);
		}

		int yes = 1;
		for( p = ai; p != NULL; p = p ->ai_next)
		{
			listener_ = socket( p->ai_family,  p->ai_socktype, p->ai_protocol);
			
			if( listener_ < 0)
				continue;

			// avoid "address already in use" by kernel error message
			setsockopt( listener_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof( int));
			
			if( bind( listener_, p->ai_addr, p->ai_addrlen) < 0)
			{
				close( listener_);
				continue;
			}
			
			break;
		}

		if( p == NULL)
		{
			fprintf( stderr, "chat_server: failed to bind\n");
			exit(2);
		}

		freeaddrinfo(ai);

		// listen
		if( listen( listener_, 10) == -1)
		{
			perror("listen");
			exit(3);
		}

		master_.add(listener_);
		
		// keep track of the biggest fd
		fdmax_ = listener_;
	}

	void
	handle_new_client_()
	{
		socklen_t addrlen;
		struct sockaddr_storage remoteaddr;
		int newfd;
		addrlen = sizeof remoteaddr;
		newfd = accept( listener_, (struct sockaddr *) &remoteaddr, &addrlen);

		if( newfd == -1)
		{
			perror( "accept");
		} else
		{
			master_.add(newfd);
			fdmax_ = (fdmax_ < newfd) ? newfd : fdmax_;
			
			char remoteIP[INET6_ADDRSTRLEN];
			printf( "chat_server: new connection from %s on socket %d\n",
				inet_ntop( remoteaddr.ss_family,
					get_in_addr_( (struct sockaddr*) &remoteaddr),
					remoteIP, INET6_ADDRSTRLEN),
				newfd);
		}
		
		send_msg_( newfd, "Username = ");

		unregistered_users_.insert( newfd);
	}

	void
	connection_closing_( int fd, int nbytes)
	{
		if( nbytes == 0)
		{
			// connection closed
			printf( "chat_server: socket %d hung up\n", fd);
		} else
		{
			// recv() < 0 -> error
			perror( "recv");
		}
		
		// let others know who left
		channel_broadcast_( fd, "<---has left the channel--->\n");
		
		close( fd);
		master_.rm( fd);
		users_.erase( users_.find( fd));
	}

	void
	assign_username_( std::set<int>::iterator it, const std::string && name)
	{
		std::cout << "username " << name << " assigned to socket " << 
			*it << std::endl;
		
		users_.insert( std::pair< int, std::string>( *it, name));
		
		// let others know who joined
		channel_broadcast_( *it, "<---has joined the channel--->\n");

		unregistered_users_.erase( it);
	}

	void
	channel_broadcast_( const int sender_fd, const std::string && msg)
	{
		std::string sender;
		if( users_.find( sender_fd) == users_.end())
		{		
			fprintf( stderr, "ERROR: users_ database compromised!\n");
			sender = "???";
		} else
		{
			sender = users_.find( sender_fd)->second;
		}
		
		
		for( int j = 0; j <= fdmax_; j++)
		{
			// send to everyone in current channel
			// 	except listener, sender and unregistered
			std::set<int>::iterator it = unregistered_users_.find( j);

			if( master_.has( j) && sender_fd != j && j != listener_ &&
				it == unregistered_users_.end())
			{
				// send channel + username + message
				send_msg_( j, "[" + name_ + "]::" + sender + "->" + msg);
			}
		}
	}

	std::string
	to_string_( char * buf, int len)
	{
			format_( &buf, len);
			return (std::string) buf;
	}

	void
	format_( char ** buf, int len)
	{
		for( int i = 0; i < len; ++i)
		{
			// any newline char terminates the message
			if( ( *buf)[ i] == 13 || (* buf)[ i] == 10)
			{
				( *buf)[ i] = '\0';
				break;
			}

			// consider only printable characters
			if( ( *buf)[ i] < 32)
			{
				( *buf)[ i] = '_';	// replace with '_'
			}
		}
	}
	
	void
	send_msg_( const int fd, const std::string && msg)
	{
		if( msg.length() > MAX_LEN)
		{
			fprintf( stderr, "Message for fd %d"
					"is truncated due to length\n", fd);
			msg.substr( MAX_LEN);
		}

		if( send( fd, msg.c_str(), msg.length(), 0) == -1)
			perror( "send_msg");
	}
};

int
main(int argc, char ** argv)
{
	const char * port = (argc == 2) ? argv[1] : "9034";	

	Channel ch1( "public", port);
	
	while(1)
	{
		ch1.process();
	}

	return 0;	
}
