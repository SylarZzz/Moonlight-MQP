//
// Created by Sylar Zhang on 1/24/2023.
//

#include <iostream>
#include "Queue.h"

//using namespace std;

void Queue::dequeue()
{
    if (q.size() > 0) {
        std::cout << "Dequeued " << q.front() << std::endl;
        q.pop();
    } else {
        std::cout << "Cannot dequeue empty queue.\n";
    }

}

void Queue::enqueue(int item)
{
    q.push(item);
    std::cout << "Enqueued " << item << std::endl;
}
