#include "mnet.h"
#include <cmath>
#include <cerrno>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <netinet/tcp.h>

// The DO_INVOKE macro is a trick to help resolve the problem of that
// during the user callback it register a new handler. However if we
// remove the current handler this will remove the newly registered
// handl

#define DO_INVOKE(X,T,...) \
    do { \
        assert(!(X).IsNull()); \
        T cb((X).Release()); \
        cb->Invoke(__VA_ARGS__); \
    } while(false)


#define VERIFY(cond) \
    do { \
        if(!(cond)) { \
            fprintf(stderr,"Assertion failed:%s,(%d:%s)",#cond,errno,strerror(errno)); \
            std::abort(); \
        } \
    } while(0)


namespace mnet {
namespace detail {
namespace {

// This function sets the file descriptors to has TCP attributes
// TCP_NODELAY
void SetTcpNoDelay( int fd ) {
    int tag = 1;
    VERIFY( ::setsockopt(fd,
                       IPPROTO_TCP,
                       TCP_NODELAY,
                       reinterpret_cast<char*>(&tag),
                       sizeof(int)) ==0 );
}

// This function sets the file as REUSE the TCP address
void SetReuseAddr( int fd ) {
    int tag = 1;
    VERIFY( ::setsockopt(fd,
                       SOL_SOCKET,
                       SO_REUSEADDR,
                       reinterpret_cast<char*>(&tag),
                       sizeof(int)) == 0 );
}

// This function creates that file descriptors and set its FD has
// 1. O_NONBLOCK 2. O_CLOEXEC
int NewFileDescriptor() {
    int fd = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if (UNLIKELY(fd <0)) 
        return -1;
    // Setting up the FCNTL
    int flag = fcntl(fd,F_GETFL);
    flag |= O_NONBLOCK;
    flag |= O_CLOEXEC;
    fcntl(fd,F_SETFL,flag);
    return fd;
}

uint64_t GetCurrentTimeInMS() {
    struct timeval tv;
    VERIFY( ::gettimeofday(&tv,NULL) == 0 );
    return tv.tv_sec * 1000 + static_cast<uint64_t>(tv.tv_usec/1000);
}
}// namespace

int CreateTcpFileDescriptor() {
    int fd = NewFileDescriptor();
    if( UNLIKELY(fd < 0) )
        return -1;
    SetTcpNoDelay(fd);
    SetReuseAddr(fd);
    return fd;
}

int CreateTcpServerSocketFileDescriptor() {
    int fd = NewFileDescriptor();
    if( UNLIKELY(fd < 0) )
        return -1;
    SetReuseAddr(fd);
    return fd;
}

}// namespace detail


void Buffer::Grow( std::size_t cap ) {
    if( UNLIKELY(cap == 0) ) {
        return;
    }
    const std::size_t sz = cap + readable_size();
    void* mem = malloc(sz);

    // Copy the readable portion of the data into the head of the
    // new memory buffer
    if( readable_size() > 0 )
        memcpy(mem,static_cast<char*>(mem_)+read_ptr_,readable_size());
    write_ptr_ = readable_size();
    read_ptr_ = 0;
    capacity_ = sz;

    // Free the old buffer
    free(mem_);
    mem_ = mem;
}

void* Buffer::Read( std::size_t* size ) {
    // Calculate the possible read size for this buffer
    const std::size_t read_sz =
        std::min(*size, readable_size() );
    void* mem = static_cast<char*>(mem_) + read_ptr_ ;

    // Update the read_ptr since Read is a mutator
    read_ptr_ += read_sz;

    RewindBuffer();
    return mem;
}

bool Buffer::Write( const void* mem , std::size_t length ) {
    // Check if we have enough space to hold this memory
    if( UNLIKELY(writable_size() < length) ) {
        if( is_fixed_ ) 
            return false;

        std::size_t ncap = length > capacity_ ? length : capacity_;
        ncap *= 2;
        // We cannot hold this buffer now, just grow the buffer here
        Grow( ncap );
    }

    memcpy(static_cast<char*>(mem_)+write_ptr_,mem,length);
    write_ptr_ += length;
    return true;
}

std::size_t Buffer::Fill( const void* mem , std::size_t length ) {
    std::size_t sz = std::min(writable_size(),length);
    if( UNLIKELY(sz == 0) )
        return 0;
    else {
        memcpy(static_cast<char*>(mem_)+write_ptr_,mem,sz);
        write_ptr_ += sz;
        return sz;
    }
}

bool Buffer::Inject( const void* mem , std::size_t length ) {
    if( UNLIKELY(writable_size() < length) ) {
        if( is_fixed_ )
            return false;
        Grow(length);
    }
    memcpy(static_cast<char*>(mem_)+write_ptr_,mem,length);
    write_ptr_ += length;
    assert( write_ptr_ == capacity_ );
    return true;
}

int Endpoint::Ipv4ToString( char* buf ) const {
    // Parsing the IPV4 into the string. The following code should
    // only work on little endian
    uint32_t c1,c2,c3,c4;

    c1 = ipv4_ &0xff;
    c2 = (ipv4_>>8)&0xff;
    c3 = (ipv4_>>16)&0xff;
    c4 = (ipv4_>>24)&0xff;

    return sprintf(buf,"%d.%d.%d.%d",c4,c3,c2,c1);
}

int Endpoint::PortToString( char* buf ) const {
    return sprintf(buf,"%d",port_);
}

int Endpoint::StringToIpv4( const char* buf ) {
    uint32_t c1,c2,c3,c4;
    int len = 0;
    char* pend;

#define PARSE(c) \
    do { \
        errno = 0; \
        (c) = static_cast<uint32_t>(std::strtol(buf+len,&pend,10)); \
        if( errno != 0 ) { \
            goto fail; \
        } \
        if( c > 255 || c <0 ) { \
            goto fail; \
        } \
    } while(false)

#define CHECK() \
    do { \
        if( *pend != '.' ) \
            goto fail; \
        len = pend-buf+1; \
    } while(false)

    // Component 1
    PARSE(c1);
    CHECK();

    // Component 2
    PARSE(c2);
    CHECK();

    // Component 3
    PARSE(c3);
    CHECK();

    // Component 4
    PARSE(c4);
    ipv4_ = (c4) | (c3<<8) | (c2<<16) | (c1<<24);
    return pend-buf;

fail:
    port_ = kEndpointError;
    return -1;

#undef PARSE
#undef CHECK
}

int Endpoint::StringToPort( const char* buf ) {
    long p;
    char* pend;

    errno = 0 ;
    p = strtol(buf,&pend,10);

    if( UNLIKELY(errno != 0) ) {
        port_ = kEndpointError;
        return -1;
    } else {
        if( UNLIKELY(p > 65535 || p < 0) ) {
            port_ = kEndpointError;
            return -1;
        } else {
            port_ = static_cast<uint32_t>(p);
        }
    }
    return (pend-buf);
}

void Socket::OnReadNotify( ) {
    set_can_read(true);
    // In order to not make the misbehavior program mess up our user space
    // memory. If we detect that the user has not registered any callback
    // function just leave the data inside of the kernel and put the states
    // of current Pollable to readable
    if( UNLIKELY(user_read_callback_.IsNull()) ) {
        return;
    } else {
        NetState state;
        std::size_t read_sz = DoRead(&state);
        if( LIKELY(state_ != CLOSING) ) {
            // Invoke the callback function
            DO_INVOKE( user_read_callback_ ,
                detail::ScopePtr<detail::ReadCallback>,
                this,read_sz,state);
        } else {
            if( LIKELY(state) ) {
                // We are in closing state, so it should be an asynchronous close
                // We may still receive data here, we need to notify the user to
                // consume the data here
                if( UNLIKELY(read_sz > 0) ) {
                    user_close_callback_->InvokeData( read_sz );
                } else {
                    // Checking whether we hit eof during the last read
                    if( eof_ ) {
                        bool deleted = false;
                        set_notify_flag( &deleted );
                        detail::ScopePtr<detail::CloseCallback> cb( user_close_callback_.Release() );
                        cb->InvokeClose(NetState());
                        if( !deleted ) {
                            Close();
                            state_ = CLOSED;
                        }
                    }
                }
            } else {
                bool deleted = false;
                set_notify_flag( &deleted );
                // We failed here, so we just go straitforward to issue an
                // Close operation on the notifier and close the underlying socket
                user_close_callback_->InvokeClose(state);
                if( !deleted ) {
                    Close();
                    state_ = CLOSED;
                }
            }
        }
    }
}

void Socket::OnWriteNotify( ) {
    // Set up the can write flag
    set_can_write(true);
    if( UNLIKELY(write_buffer().readable_size() == 0) ) {
        // We do nothing since we have nothing to write out
        return;
    } else {
        NetState write_state;
        std::size_t write_sz = DoWrite(&write_state);
        if( LIKELY(write_state) ) {
            // We don't have an error just check if we hit the buffer size
            if( write_buffer().readable_size() == 0 ) {
                // We have written all the data into the underlying socket
                DO_INVOKE(user_write_callback_,
                        detail::ScopePtr<detail::WriteCallback>,
                        this,
                        prev_write_size_ + write_sz , write_state );
            } else {
                prev_write_size_ += write_sz;
            }
        } else {
            DO_INVOKE(user_write_callback_,
                    detail::ScopePtr<detail::WriteCallback>,
                    this,
                    prev_write_size_, write_state );
        }
    }
}

void Socket::OnException( const NetState& state ) {
    assert( !state );
    bool deleted = false;
    set_notify_flag( &deleted );

    if( LIKELY(!user_read_callback_.IsNull()) ) {
        DO_INVOKE(user_read_callback_,
                  detail::ScopePtr<detail::ReadCallback>,
                  this,0,state);
    }
    if( !deleted ) {
        if( LIKELY(!user_write_callback_.IsNull()) ) {
            DO_INVOKE(user_write_callback_,
                    detail::ScopePtr<detail::WriteCallback>,
                    this,0,state);
        }
    }
}

std::size_t Socket::DoRead( NetState* ok ) {
    // Whenevenr this function gets called, we need to look into the
    // kernel since this means that user wants data.
    struct iovec buf[2];
    std::size_t read_sz = 0;
    // Clear the NetState structure
    ok->Clear();

    // When we are see eof, we will always spin on this states unless
    // user use Close/AsyncClose to move the socket state to correct one
    if( UNLIKELY(eof_) ) {
        return 0;
    }
    
    do {
        // Using a loop to force us run into the EAGAIN/EWOULDBLOCK

        // The iovec will contain following structure. The first component
        // of that buffer is pointed to the extra(free) buffer in our read
        // buffer. The second component is pointed to our stack buffer. In
        // most cases this one readv will read up all the data in the kernel
        // since the system call time is way less than the packet transfer
        // time. Assume epoll_wait will wake up once a fd recieve a packet.

        Buffer::Accessor accessor = read_buffer().GetWriteAccessor();

        // Setting up the first component which points to the extra write space
        buf[0].iov_base = accessor.address();
        buf[0].iov_len = accessor.size();

        // Setting up the stack iovec component
        buf[1].iov_base = io_manager_->swap_buffer_;
        buf[1].iov_len = io_manager_->swap_buffer_size_;

        // Start to read
        ssize_t sz = ::readv( fd() , buf , 2 );

        if( sz < 0 ) {
            // Error happened
            if( LIKELY(errno == EAGAIN || errno == EWOULDBLOCK) ) {
                set_can_read(false);
                return read_sz;
            } else {
                if( errno == EINTR )
                    continue;
                // The current error is not recoverable, we just return with an error
                // states here
                ok->CheckPoint(state_category::kSystem,errno);
                return read_sz;
            }
        } else {
            if( UNLIKELY(sz == 0) ) {
                // We have seen an EOF flag, however for this user reading
                // operations, we will not being able to do this, we set
                // our socket flag to SEE_EOF , later on we can replay this
                // eof to user
                eof_ = true;
                return read_sz;
            } else {
                const std::size_t accessor_sz = accessor.size();

                if( static_cast<std::size_t>(sz) <= accessor_sz ) {
                    accessor.set_committed_size( sz );
                } else {
                    // The kernel has written data into the second stack buffer
                    // now we need to grow our buffer by using write operations
                    accessor.set_committed_size( accessor_sz );
                    accessor.Commit();

                    // Inject the data into the buffer, this injection will not
                    // cause buffer overhead since they just write the data without
                    // preallocation
                    if( !read_buffer().Inject( io_manager_->swap_buffer_ , sz-accessor_sz ) ) {
                        *ok = NetState(state_category::kSystem,ENOBUFS);
                        return read_sz;
                    }
                }
                read_sz += sz;

                if( static_cast<std::size_t>(sz) < io_manager_->swap_buffer_size_ + accessor_sz ) {
                    set_can_read(false);
                    return read_sz;
                } else {
                    continue;
                }
            }
        }
    } while(true); 
}

std::size_t Socket::DoWrite( NetState* ok ) {
    assert( write_buffer().readable_size() > 0 );
    ok->Clear();
    do {
        // Start to write the data
        Buffer::Accessor accessor = write_buffer().GetReadAccessor();

        // Trying to send out the data to underlying TCP socket
        ssize_t sz = ::write(fd(),accessor.address(),accessor.size());

        // Write can return zero which has same meaning with negative
        // value( I guess this is for historic reason ). What we gonna
        // do is that we will treat zero and -1 as same stuff and check
        // the errno value
        if( UNLIKELY(sz <= 0) ) {
            if( LIKELY(errno == EAGAIN || errno == EWOULDBLOCK) ) {
                // This is a partial operation, we need to wait until epoll_wait
                // to wake me up
                set_can_write(false);
                return 0;
            } else {
                if( errno == EINTR )
                    continue;
                // Set up the error object and record the error string
                ok->CheckPoint(state_category::kSystem,errno);
                // Return the size of the data has been sent to the kernel
                return prev_write_size_;
            }
        } else {
            // Set up the committed size
            if( static_cast<std::size_t>(sz) < accessor.size() ) {
                set_can_write(false);
            }
            accessor.set_committed_size( static_cast<std::size_t>(sz) );
            return static_cast<std::size_t>(sz);
        }
    } while(true);
}

void Socket::GetLocalEndpoint( Endpoint* endpoint ) {
    struct sockaddr_in ipv4;
    bzero(&ipv4,sizeof(ipv4));
    ipv4.sin_family = AF_INET;
    socklen_t sz = sizeof(ipv4);

    VERIFY( ::getsockname(fd(),
                reinterpret_cast<struct sockaddr*>(&ipv4),&sz) == 0);

    // writing the data into the endpoint representation
    endpoint->set_port( ntohs(ipv4.sin_port) );
    endpoint->set_ipv4( ntohl(ipv4.sin_addr.s_addr) );
}

void Socket::GetPeerEndpoint( Endpoint* endpoint ) {
    struct sockaddr_in ipv4;
    bzero(&ipv4,sizeof(ipv4));
    ipv4.sin_family = AF_INET;
    socklen_t sz = sizeof(ipv4);

    VERIFY( ::getpeername(fd(),
                reinterpret_cast<struct sockaddr*>(&ipv4),&sz) == 0);

    endpoint->set_port( ntohs(ipv4.sin_port) );

    endpoint->set_ipv4( ntohl(ipv4.sin_addr.s_addr) );
}

void ClientSocket::OnReadNotify( ) {
    if( LIKELY(state_ == CONNECTED) ) {
        Socket::OnReadNotify();
        return;
    } else {
        switch( state_ ) {
            case DISCONNECTED:

                // When disconneted socket has any notifiaction
                // just ignore it, maybe register a log information
                return;
            case CONNECTING:
                // Read, for connecting, the read information is unrelated
                // even if we receive it( we should not ), we just ignore
                return;
            default:
                UNREACHABLE(return);
        }
    }
}

void ClientSocket::OnWriteNotify() {
    if( LIKELY(state_ == CONNECTED) ) {
        Socket::OnWriteNotify();
        return;
    } else {
        if( state_ == CONNECTING ) {
            set_can_write(true);
            state_ = CONNECTED;
            DO_INVOKE(user_conn_callback_,
                      detail::ScopePtr<detail::ConnectCallback>,
                      this,NetState());
        }
    }
}

void ClientSocket::OnException( const NetState& state ) {
    assert( !state );
    if( LIKELY(state_ == CONNECTED) ) {
        Socket::OnException(state);
    } else {
        if( state_ == CONNECTING ) {
            state_ = DISCONNECTED;
            if( UNLIKELY(!user_conn_callback_.IsNull()) ) {
                DO_INVOKE(user_conn_callback_,
                          detail::ScopePtr<detail::ConnectCallback>,
                          this,state);
            }
        }
    }
}

bool ServerSocket::Bind( const Endpoint& endpoint ) {
    assert( is_bind_ == false );
    // Setting up the listener file descriptors
    int sock_fd = detail::CreateTcpServerSocketFileDescriptor();
    if( UNLIKELY(sock_fd < 0) ) {
        return false;
    }
    set_fd( sock_fd );

    // Set up the struct sockaddr_in
    struct sockaddr_in ipv4;
    bzero(&ipv4,sizeof(ipv4));

    ipv4.sin_family = AF_INET;
    ipv4.sin_addr.s_addr = htonl(endpoint.ipv4());
    ipv4.sin_port = htons(endpoint.port());

    // Bind the address
    int ret = ::bind( fd(),
            reinterpret_cast<struct sockaddr*>(&ipv4) , sizeof(ipv4) );
    if( UNLIKELY(ret != 0) ) {
        ::close(fd());
        set_fd(-1);
        return false;
    }

    // Set the fd as listen fd
    ret = ::listen( fd() , SOMAXCONN );
    if( UNLIKELY(ret != 0) ) {
        ::close(fd());
        set_fd(-1);
        return false;
    }

    is_bind_ = true;
    return true;
}

void ServerSocket::HandleRunOutOfFD( int err ) {
    // Handling the run out of file descriptors error here, if we
    // don't kernel will not free any resource but continue bothering
    // us for this problem.
    switch( err ) {
        case EMFILE:
        case ENFILE: {
            int f;
            // Run out the file descriptors and gracefully shutdown the peer side
            VERIFY( ::close( dummy_fd_ ) == 0 );
            f = ::accept(fd(),NULL,NULL);
            if( f > 0 )
                VERIFY( ::close( f ) == 0 );
            VERIFY( (dummy_fd_ = ::open("/dev/null",O_RDONLY)) >= 0 );
        } 
        default: return;
    }
    UNREACHABLE(return);
}

int ServerSocket::DoAccept( NetState* state ) {
    assert( can_read() );
    do {
        int nfd = ::accept4( fd() , NULL , NULL , O_CLOEXEC | O_NONBLOCK );
        if( UNLIKELY(nfd < 0) ) {
            if( LIKELY(errno == EAGAIN || errno == EWOULDBLOCK) ) {
                set_can_read(false);
                return -1;
            } else {
                if( errno == EINTR ) 
                    continue;
                int err = errno;
                HandleRunOutOfFD( err );
                state->CheckPoint(state_category::kSystem,err);
                if( errno == EAGAIN || errno == EWOULDBLOCK ) {
                    set_can_read(false);
                }
                return -1;
            }
        } else {
            return nfd;
        }
    } while( true );
}

void ServerSocket::OnReadNotify() {
    assert( is_bind_ );
    set_can_read( true );
    if( UNLIKELY(user_accept_callback_.IsNull()) )
        return;
    else {
        NetState accept_state;

        int nfd = DoAccept(&accept_state);
        if( UNLIKELY(nfd < 0) ) {
            if( !accept_state ) {
                DO_INVOKE( user_accept_callback_ ,
                        detail::ScopePtr<detail::AcceptCallback>,
                        new_accept_socket_,accept_state);
                new_accept_socket_ = NULL;
            }
        } else {
            detail::Pollable* p = static_cast<detail::Pollable*>(new_accept_socket_);
            p->set_fd( nfd );

            // Temporarily store the new_accept_socket_ to enable user seting it during
            // the invocation of the user_accept_callback_ function

            Socket* s = new_accept_socket_;
            new_accept_socket_ = NULL ;

            DO_INVOKE( user_accept_callback_ ,
                    detail::ScopePtr<detail::AcceptCallback>,
                    s, NetState());
        }
    }
}

void ServerSocket::OnException( const NetState& state ) {
    HandleRunOutOfFD( state.error_code() );
    // We have an exception on the listener socket file descriptor
    if( !user_accept_callback_.IsNull() ) {
        DO_INVOKE( user_accept_callback_ ,
                   detail::ScopePtr<detail::AcceptCallback>,
                   new_accept_socket_,state);
    }
}

ServerSocket::ServerSocket() :
    new_accept_socket_(NULL),
    io_manager_(NULL),
    is_bind_( false )
{
    // Initialize the dummy_fd_ here
    dummy_fd_ = ::open("/dev/null", O_RDONLY );
    VERIFY( dummy_fd_ >= 0 );
}

ServerSocket::~ServerSocket() {
    // Closing the listen fd
    VERIFY( ::close(fd()) == 0 );
    set_fd(-1);
    ::close( dummy_fd_ );
}

IOManager::IOManager( std::size_t cap ) {
    epoll_fd_ = ::epoll_create1( EPOLL_CLOEXEC );
    VERIFY( epoll_fd_ > 0 );

    // Set up the control file descriptors. This file descriptor will
    // be set up as a udp socket just because it is simple.
    int fd = socket( AF_INET , SOCK_DGRAM , 0 );
    VERIFY(fd >0);

    // Set up the flag for this file descriptor
    int flag = ::fcntl(fd,F_GETFL);
    flag |= O_CLOEXEC;
    flag |= O_NONBLOCK;
    ::fcntl(fd,F_SETFL);

    // Setup the bind for the control file descriptor
    struct sockaddr_in ipv4;
    ipv4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ipv4.sin_family = AF_INET;
    ipv4.sin_port = htons(0);
    VERIFY( ::bind(fd,reinterpret_cast<struct sockaddr*>(&ipv4),sizeof(ipv4)) == 0 );
    // Set this newly created file descriptor back to the CtrlFd object
    ctrl_fd_.fd_ = fd;

    // Now we need to watch the read operation for this UDP socket.
    WatchRead(&ctrl_fd_);

    if( cap == 0 ) {
        static const std::size_t kDefaultRecvBufferSize = 3495200;
        cap = kDefaultRecvBufferSize;
    } 

    swap_buffer_ = malloc(cap);
    swap_buffer_size_ = cap;

}

IOManager::~IOManager() {
    if( epoll_fd_ > 0 ) {
        ::close(ctrl_fd_.fd_);
        ctrl_fd_.set_fd(-1);
        ::close(epoll_fd_);
    }
    // Check if we have timer queue problem
    for( std::size_t i = 0 ; i < timer_queue_.size() ; ++i ) {
        delete timer_queue_[i].callback;
    }

    free(swap_buffer_);
}

void IOManager::CtrlFd::OnReadNotify() {
    // We will ignore the error once our control file descriptor receive notification
    // since no matter is correct notification or not (it should in most case do not
    // have an error state), the IOManager needs to be waked up.
    char buf[kDataLen];
    // If this state is correct we read up the information inside of it and
    // hit the EAGAIN/EWOULDBLOCK
    ::recvfrom( fd() , buf , kDataLen , 0 , NULL , NULL );
    is_wake_up_ = true;
}

void IOManager::Interrupt() {
    struct sockaddr_in ipv4;
    bzero(&ipv4,sizeof(ipv4));
    ipv4.sin_family = AF_INET;
    socklen_t ipv4_len = sizeof(ipv4);

    int ret = ::getsockname( ctrl_fd_.fd(),
                reinterpret_cast<struct sockaddr*>(&ipv4) , &ipv4_len );

    VERIFY(ret == 0);

    // Random bytes
    char buf[CtrlFd::kDataLen];

    // Send the data to that UDP socket
    VERIFY( ::sendto(ctrl_fd_.fd(),buf,CtrlFd::kDataLen,0,
                reinterpret_cast<struct sockaddr*>(&ipv4),sizeof(ipv4)) == CtrlFd::kDataLen );
}


// Through epoll watch the pollable descriptor's read/write operation
// there's no extra space usage , the pointer for Pollable are stored
// inside of the epoll_data structure per registeration

void IOManager::WatchRead( detail::Pollable* pollable ) {
    assert( pollable->Valid() );
    // We don't remove any file descriptors unless user explicitly require so
    if( LIKELY(pollable->is_epoll_read_) )
        return;

    struct epoll_event ev;
    int op;

    ev.data.ptr = pollable;

    // Edge trigger for read
    ev.events = EPOLLIN | EPOLLET ;

    if( UNLIKELY(pollable->is_epoll_write_) ) {
        op = EPOLL_CTL_MOD;
        ev.events |= EPOLLOUT;
    } else {
        op = EPOLL_CTL_ADD;
    }

    VERIFY( ::epoll_ctl( epoll_fd_ , op , pollable->fd_ , &ev ) == 0 );

    // Set up we gonna watch it
    pollable->is_epoll_read_ = true;

}

void IOManager::WatchWrite( detail::Pollable* pollable ) {
    assert( pollable->Valid() );
    if( LIKELY(pollable->is_epoll_write_) )
        return;

    struct epoll_event ev;
    int op;

    ev.data.ptr = pollable;
    ev.events = EPOLLOUT | EPOLLET ;

    if( UNLIKELY(pollable->is_epoll_read_) ) {
        op = EPOLL_CTL_MOD;
        ev.events |= EPOLLIN;
    } else {
        op = EPOLL_CTL_ADD;
    }

    VERIFY( ::epoll_ctl( epoll_fd_ , op , pollable->fd_ , &ev ) == 0 );
    pollable->is_epoll_write_ = true;
}

void IOManager::WatchControlFd() {
    struct epoll_event ev;
    ev.data.ptr = &ctrl_fd_;
    ev.events = EPOLLIN;
    VERIFY( ::epoll_ctl( epoll_fd_ , EPOLL_CTL_ADD , ctrl_fd_.fd_ , &ev ) == 0 );
}

void IOManager::DispatchLoop( const struct epoll_event* event_queue , std::size_t sz ) {
    for( std::size_t i = 0 ; i < sz ; ++i ) {
        detail::Pollable* p = static_cast<detail::Pollable*>(event_queue[i].data.ptr);
        int ev = event_queue[i].events;

        // Handling error
        if( UNLIKELY(ev & EPOLLERR) ) {
            // Get the per socket error here
            socklen_t len = sizeof(int);
            int err_no;
            VERIFY( ::getsockopt(p->fd_,SOL_SOCKET,SO_ERROR,&err_no,&len) == 0 );
            if( err_no != 0 ) {
                p->OnException( NetState(state_category::kSystem,err_no) );
                continue;
            }
            ev &= ~EPOLLERR;
        }

        if( UNLIKELY(event_queue[i].events & EPOLLHUP) ) {
            // Translate it into a read event
            p->OnReadNotify();
            continue;
        }

        // IN/OUT events
        bool deleted = false;
        p->set_notify_flag( &deleted );

        if( LIKELY(event_queue[i].events & EPOLLIN) ) {
            p->OnReadNotify();
            ev &= ~EPOLLIN;
        }

        if( LIKELY(event_queue[i].events & EPOLLOUT) ) {
            if( !deleted ) 
                p->OnWriteNotify();
            ev &= ~EPOLLOUT;
        }
        // We may somehow have unwatched event here.
        // We can log them for debuggin or other stuff
        VERIFY( ev == 0 );
    }
}

uint64_t IOManager::UpdateTimer( std::size_t event_sz , uint64_t prev_time ) {
    static const int kMinDiff = 3;
#define TIME_TRIGGER(diff,t) (std::abs((t)-diff) < kMinDiff)

    if( !timer_queue_.empty() ) {
        if( event_sz == 0 ) {
            uint64_t diff = timer_queue_.front().time;
            while( !timer_queue_.empty() ) {
                if( LIKELY(TIME_TRIGGER(diff,timer_queue_.front().time)) ) {
                    detail::ScopePtr<detail::TimeoutCallback> cb(
                        timer_queue_.front().callback);

                    cb->Invoke(timer_queue_.front().time);
                    // Pop from the top element from the heap
                    std::pop_heap( timer_queue_.begin() , timer_queue_.end() );
                    timer_queue_.pop_back();

                } else {
                    break;
                }
            }
            return detail::GetCurrentTimeInMS();
        } else {
            int ret = detail::GetCurrentTimeInMS();
            int diff = static_cast<int>( ret - prev_time );
            for( std::size_t i = 0 ; i < timer_queue_.size() ; ++i ) {
                timer_queue_[i].time -= diff;
            }
            return ret;
        }
    }
#undef TIME_TRIGGER
}

void IOManager::ExecutePendingAccept() {
    while( !pending_accept_callback_.IsNull() ) {
        DO_INVOKE( pending_accept_callback_,
                detail::ScopePtr<detail::AcceptCallback>,
                new_accept_socket_,pending_accept_state_);
    }
}

NetState IOManager::RunMainLoop() {
    struct epoll_event event_queue[ IOManager::kEpollEventLength ];
    uint64_t prev_time = detail::GetCurrentTimeInMS();
    do {
        // 0. Execute pending accept
        ExecutePendingAccept();
        // 1. Set up the parameter that we need for the epoll_wait , it is very simple
        int tm = timer_queue_.empty() ? -1 : timer_queue_.front().time ;

repoll:
        int ret = ::epoll_wait( epoll_fd_ , event_queue , kEpollEventLength , tm );

        if( UNLIKELY(ret < 0) ) {

            if( LIKELY(errno != EINTR) )
                return NetState(state_category::kSystem,errno);
            else
                // We don't need to go to the begining of the loop since this will cause us
                // to reflush the timer there. Goto repoll label to start another epoll_wait
                // would be easiest way we can do
                goto repoll;
        } else {
            // Do dispatch for the event here
            DispatchLoop( event_queue , static_cast<std::size_t>( ret ) );
            // Update or invoke the timer event.
            prev_time = UpdateTimer( static_cast<std::size_t>(ret) , prev_time );
            // Checking whether we have been notified by interruption
            if( UNLIKELY(ctrl_fd_.is_wake_up()) ) {
                // We have been waken up by the caller, just return empty
                // NetState here
                return NetState();
            }
        }
    } while( true );
}

}// namespace mnet

