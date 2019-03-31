#include "udp_socket.hpp"

UdpSocket:: UdpSocket(std::string addr, std::string port, int bind_op){

    sock =  socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
    unsigned int length = sizeof(struct sockaddr_in);

    socksaddr.sin_family = AF_INET;
    socksaddr.sin_addr.s_addr = inet_addr(addr.c_str());
    socksaddr.sin_port = htons(atoi(port.c_str()));

    if(bind_op == 1){
        if(bind(sock, (struct sockaddr *)(&socksaddr),length) == -1){

            printf("Can't bind address (%s:%s)\n", addr.c_str(), port.c_str());
            exit(1);

        }
        else{
            
            printf("Binding successful (%s:%s)\n", addr.c_str(), port.c_str());
        }
    }

}

UdpSocket::UdpSocket(std::string complete_address, int bind_op){

    sock =  socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
    unsigned int length = sizeof(struct sockaddr_in);

    std::string addr = readAddress(complete_address);
    std::string port = readPort(complete_address);
    
    socksaddr.sin_family = AF_INET;
    socksaddr.sin_addr.s_addr = inet_addr(addr.c_str());
    socksaddr.sin_port = htons(atoi(port.c_str()));

    if(bind_op == 1){
        if(bind(sock, (struct sockaddr *)(&socksaddr),length) == -1){

            printf("Can't bind address (%s:%s)\n", addr.c_str(), port.c_str());
            exit(1);

        }
        else{
            
            printf("Binding successful (%s:%s)\n", addr.c_str(), port.c_str());
        }
    }

}

int UdpSocket::receivePacket(){
    
    unsigned int length = sizeof(struct sockaddr_in);

    int n = recvfrom(sock,buffer,sizeof(buffer),0,(struct sockaddr *)(&socksaddr),&length);

    return n;
}

int UdpSocket::receivePacket(uint8_t *& in_buffer,size_t buffer_size){

    unsigned int length = sizeof(struct sockaddr_in);

    int n = recvfrom(sock,in_buffer,buffer_size,0,(struct sockaddr *)(&socksaddr),&length);

    return n;

}

void UdpSocket::sendPacket(void* in_buffer, size_t buffer_size){

    unsigned int length = sizeof(struct sockaddr_in);

    sendto(sock, in_buffer, buffer_size,0,(struct sockaddr *)(&socksaddr),length);

}

std::string UdpSocket::getAddress(){

    return addr;
}

std::string UdpSocket::getPort(){

    return port;
}

void* UdpSocket::getBuffer(){

    return buffer;
}

std::string UdpSocket::readPort(std::string str){
 
    str.erase( str.begin() ,str.begin() + str.find(":")+1);

    return str;

}

std::string UdpSocket::readAddress(std::string str){
 
    return str.erase(str.find(":"),str.size());
}