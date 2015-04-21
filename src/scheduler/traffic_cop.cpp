/*-------------------------------------------------------------------------
 *
 * traffic_cop.cpp
 * file description
 *
 * Copyright(c) 2015, CMU
 *
 * /n-store/src/backend/traffic_cop.cpp
 *
 *-------------------------------------------------------------------------
 */

#include <tbb/tbb.h>
#include <tbb/concurrent_queue.h>

#include "scheduler/traffic_cop.h"
#include <iostream>

namespace nstore {
namespace scheduler {

std::istream& operator >> (std::istream& in, Payload& msg) {

  int type = PAYLOAD_TYPE_INVALID;
  std::cin >> type;

  std::getline (std::cin, msg.data);
  msg.msg_type = (PayloadType) type;

  return in;
};

//===--------------------------------------------------------------------===//
// Traffic Cop
//===--------------------------------------------------------------------===//

void TrafficCop::Execute() {
  Payload msg;

  for(;;) {
    std::cin >> msg;

    switch(msg.msg_type){
      case PAYLOAD_TYPE_CLIENT_REQUEST:
        std::cout << "Request :: " << msg.data << "\n";

        //LongTask* t = new( tbb::task::allocate_root() ) LongTask(hWnd);
        //tbb::task::enqueue(*t);
        break;

      case PAYLOAD_TYPE_STOP:
        std::cout << "Stopping server.\n";
        exit(EXIT_SUCCESS);

      case PAYLOAD_TYPE_INVALID:
      default:
        std::cout << "Unknown message type : " << msg.msg_type << "\n";
        exit(EXIT_SUCCESS);
    }
  }

}

} // namespace scheduler
} // namespace nstore


