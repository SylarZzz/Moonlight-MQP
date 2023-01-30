//
// Created by Sylar Zhang on 1/24/2023.
//
#include <iostream>
#include "Queue.h"
using namespace std;

void Queue::dequeue()
{
    if (q.size() > 0) {
        cout << "Dequeued " << q.front() << endl;
        q.pop();
    } else {
        cout << "Cannot dequeue empty queue.\n";
    }

}

void Queue::enqueue(int item)
{
    q.push(item);
    cout << "Enqueued " << item << endl;
}
