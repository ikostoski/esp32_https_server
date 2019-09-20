#include "HTTPServer.hpp"

namespace httpsserver {


HTTPServer::HTTPServer(const uint16_t port, const uint8_t maxConnections, const in_addr_t bindAddress):
  _port(port),
  _maxConnections(maxConnections),
  _bindAddress(bindAddress) {

  // Create space for the connections
  _connections = new HTTPConnection*[maxConnections];
  for(uint8_t i = 0; i < maxConnections; i++) _connections[i] = NULL;

  // Configure runtime data
  _socket = -1;
  _pendingQueue = xQueueCreate(1, sizeof(int));
  _pendingConnection = false;
  _running = false;
}

HTTPServer::~HTTPServer() {

  // Stop the server.
  // This will also remove all existing connections
  if(_running) {
    stop();
  }

  // Delete connection pointers
  delete[] _connections;
}

/**
 * This method starts the server and begins to listen on the port
 */
uint8_t HTTPServer::start() {
  if (!_running) {
    if (setupSocket()) {
      _running = true;
      return 1;
    }
    return 0;
  } else {
    return 1;
  }
}

bool HTTPServer::isRunning() {
  return _running;
}

/**
 * This method stops the server
 */
void HTTPServer::stop() {

  if (_running) {
    // Set the flag that the server is stopped
    _running = false;

    // Clean up the connections
    bool hasOpenConnections = true;
    while(hasOpenConnections) {
      hasOpenConnections = false;
      for(int i = 0; i < _maxConnections; i++) {
        if (_connections[i] != NULL) {
          _connections[i]->closeConnection();

          // Check if closing succeeded. If not, we need to call the close function multiple times
          // and wait for the client
          if (_connections[i]->isClosed()) {
            delete _connections[i];
            _connections[i] = NULL;
          } else {
            hasOpenConnections = true;
          }
        }
      }
      delay(1);
    }

    teardownSocket();

  }
}

/**
 * Adds a default header that is included in every response.
 *
 * This could be used for example to add a Server: header or for CORS options
 */
void HTTPServer::setDefaultHeader(std::string name, std::string value) {
  _defaultHeaders.set(new HTTPHeader(name, value));
}

/**
 * The loop method can either be called by periodical interrupt or in the main loop and handles processing
 * of data
 * 
 * If timeout (in millisecods) is supplied, it will wait for event 
 * on the 'server soceket' for new connection and/or on established 
 * connection sockets for closing/error.
 * 
 * Return value is remaining milliseconds if funtion returned early
 */
int HTTPServer::loop(int timeoutMs) {

  // Only handle requests if the server is still running
  if (!_running) {
    delay(timeoutMs);
    return 0;
  }

  uint32_t startMs = millis();

  // Setup fd_sets to check the listerner socket for new connections
  int max_fd = -1;

  // Step 1: Process existing connections
  // Process open connections and store the index of a free connection
  // (we might use that later on)
  fd_set except_fds;
  FD_ZERO(&except_fds);
  int freeConnectionIdx = -1;
  for (int i = 0; i < _maxConnections; i++) {
    // Fetch a free index in the pointer array
    if (_connections[i] == NULL) {
      freeConnectionIdx = i;
    } else {
      // if there is a connection (_connections[i]!=NULL), check if its open or closed:
      if (_connections[i]->isTerminated()) {
        // if it's closed, clean up:
        delete _connections[i];
        _connections[i] = NULL;
        freeConnectionIdx = i;
      } else {
        // Add the connection to the except_fds set
        // to wakeup from select if connection is closed
        int fd = _connections[i]->_socket;
        FD_SET(fd, &except_fds);
        if (fd > max_fd) max_fd = fd;
        #ifndef HTTPS_TASK_PER_CONNECTION
        // and process it
        _connections[i]->loop();
        #endif
      }
    }
  }

  // Step 2: If we already have know pending connection
  // and slot for it, accept it...
  if (_pendingConnection && (freeConnectionIdx > -1)) {
    startConnection(freeConnectionIdx);
    freeConnectionIdx = -1;
    // Check again if we have open connection slot
    // to avoid terminating another idle connection
    for (int i = 0; i < _maxConnections; i++) {
      if (_connections[i] == NULL) freeConnectionIdx = i;
    }
  } 

  fd_set read_fds;
  FD_ZERO(&read_fds);
  if (!_pendingConnection) {
    // Add listener socket to read_fds set
    FD_SET(_socket, &read_fds);
    if (_socket > max_fd) max_fd = _socket;
  }
 
  // Step 3: Check for new connections or wakeup
  // if existing connection is closed
  // Uses the timeoutMs value as timeout (miliseconds)
  timeval timeout;
  timeout.tv_sec  = (timeoutMs / 1000);
  timeout.tv_usec = (timeoutMs - timeout.tv_sec) * 1000; 

  // Wait for event on descriptors
  int nfds = select(max_fd+1, &read_fds, NULL, &except_fds, &timeout);

  // If we have new connection on the listener socket
  if ((nfds > 0) && FD_ISSET(_socket, &read_fds)) {
    // And if there is space to accept the connection
    if (freeConnectionIdx > -1) {
      startConnection(freeConnectionIdx);
    } else if (!_pendingConnection) {
      // No space for connections and no exisitng pending connection.
      // Set the guard flag and put the listener socket (anything actually) 
      // into _pendingQueue so idle connection task can pick it up.
      HTTPS_LOGD("Notifying tasks that there is pending connection");
      _pendingConnection = true;
      xQueueSend(_pendingQueue, &_socket, 0);
    }
  }

  // Return the remaining time from the timeoutMs requested
  uint32_t deltaMs = (startMs + timeoutMs - millis());
  if (deltaMs > 0x7FFFFFFF) deltaMs = 0;
  return deltaMs;
}

/**
 * Start new connection with index idx and 
 * reset pending connection flag and queue
 */
int HTTPServer::startConnection(int idx) {
  // Remove pending guard flag
  _pendingConnection = false;
  // Also remove item from the pending queue, if there is one
  int dummy;
  xQueueReceive(_pendingQueue, &dummy, 0);
  int socketIdentifier = createConnection(idx);
  if (socketIdentifier < 0) {
    // initializing did not work, discard the new socket immediately
    delete _connections[idx];
    _connections[idx] = NULL;
  } 
  return socketIdentifier;
}

int HTTPServer::createConnection(int idx) {
  HTTPConnection * newConnection = new HTTPConnection(this);
  _connections[idx] = newConnection;
  newConnection->initialize(_socket, &_defaultHeaders);  
  #ifdef HTTPS_TASK_PER_CONNECTION
  return startConnectionTask(newConnection, "HTTPConn");
  #else
  return newConnection->acceptConnection();  
  #endif
}

#ifdef HTTPS_TASK_PER_CONNECTION
/** 
 * Start connection task
 */ 
int HTTPServer::startConnectionTask(HTTPConnection * connection, const char* name) {
  connection->_pendingQueue = _pendingQueue;
  BaseType_t taskRes = xTaskCreate(
    connectionTask, 
    name, 
    HTTPS_CONN_TASK_STACK_SIZE,
    connection,
    HTTPS_CONN_TASK_PRIORITY,
    &connection->_taskHandle
  );
  if (taskRes != pdTRUE) {
    HTTPS_LOGE("Error starting connection task");
    return -1;
  }
  HTTPS_LOGD("Started connection task %p", connection->_taskHandle);
  return 1; // We don't know the socket ID here
}

/**
 * This static method accepts the connection amd than calls
 * continuosLoop method until connection is closed
 */
void HTTPServer::connectionTask(void* param) {
  HTTPConnection* connection = static_cast<HTTPConnection*>(param);
  int res = connection->acceptConnection();
  if (res > 0) {
    connection->continuosLoop();
  } else {
    HTTPS_LOGE("Faled to accept connection in task %p", connection->_taskHandle);    
  }
  HTTPS_LOGD("Ending connection task %p (FID=%d)", connection->_taskHandle, res);
  connection->_taskHandle = NULL;
  vTaskDelete(NULL);
}
#endif


/**
 * This method prepares the tcp server socket
 */
uint8_t HTTPServer::setupSocket() {
  // (AF_INET = IPv4, SOCK_STREAM = TCP)
  _socket = socket(AF_INET, SOCK_STREAM, 0);

  if (_socket>=0) {
    _sock_addr.sin_family = AF_INET;
    // Listen on all interfaces
    _sock_addr.sin_addr.s_addr = _bindAddress;
    // Set the server port
    _sock_addr.sin_port = htons(_port);

    // Now bind the TCP socket we did create above to the socket address we specified
    // (The TCP-socket now listens on 0.0.0.0:port)
    int err = bind(_socket, (struct sockaddr* )&_sock_addr, sizeof(_sock_addr));
    if(!err) {
      err = listen(_socket, _maxConnections);
      if (!err) {
        return 1;
      } else {
        close(_socket);
        _socket = -1;
        return 0;
      }
    } else {
      close(_socket);
      _socket = -1;
      return 0;
    }
  } else {
    _socket = -1;
    return 0;
  }
 
}

void HTTPServer::teardownSocket() {
  // Close the actual server sockets
  close(_socket);
  _socket = -1;
}

} /* namespace httpsserver */
