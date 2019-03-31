#ifndef UDP_SOCKET_H
#define UDP_SOCKET_H

#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h> 
#include <sys/types.h>
#include <unistd.h>
#include <netdb.h>
#include <iostream>
#include <string.h>

#define h_addr h_addr_list[0] /* for backward compatibility */

class UdpSocket{

    struct sockaddr_in socksaddr;
    struct hostent *hp;
    int sock;
    char buffer[65535];

    std::string addr;
    std::string port;

    std::string readPort(std::string str);  

    std::string readAddress(std::string str);

    public:
    
    UdpSocket(std::string addr, std::string port, int bind_op);
    UdpSocket(std::string complete_address, int bind_op);

    int receivePacket();
    int receivePacket(uint8_t *& in_buffer,size_t buffer_size);
    void sendPacket(void* in_buffer, size_t buffer_size);

    void* getBuffer();

    std::string getAddress();

    std::string getPort();

    


};

#endif