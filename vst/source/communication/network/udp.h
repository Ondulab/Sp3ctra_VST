
//
//  udp.h
//  SSS_Viewer
//
//  Created by Zhonx on 16/12/2023.
//

#ifndef udp_h
#define udp_h

#include <stdio.h>
#include <netinet/in.h> /* struct sockaddr_in (Windows: compat → winsock2) */

int udp_Init(struct sockaddr_in *si_other, struct sockaddr_in *si_me);
void udp_cleanup(int socket_fd);

#endif /* udp_h */
