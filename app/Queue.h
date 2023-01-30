//
// Created by Sylar Zhang on 1/24/2023.
//

#include <queue>
using namespace std;

#ifndef UNTITLED4_QUEUE_H
#define UNTITLED4_QUEUE_H


class Queue
{

private:
    queue<int> q;

public:
    void dequeue();
    void enqueue(int x);
    int peek() { return q.front(); }
    int last() { return  q.back(); }
    int size() { return q.size(); }
    bool isEmpty() { return q.empty(); }
};


#endif //UNTITLED4_QUEUE_H
