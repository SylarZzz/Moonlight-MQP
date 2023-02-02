#ifndef QUEUE_H
#define QUEUE_H

#include "Limelight.h"
#endif // QUEUE_H

typedef struct node {
    void* data;
    struct node *next;
} Node;

typedef struct {
    Node *head, *tail;
    int size;
} Queue;

Queue *createQueue();
bool isEmpty(Queue *queue);
void enqueue(Queue *queue, void * data);
void dequeue(Queue *queue);
int front(Queue *queue);
int back(Queue *queue);
//void queueTest();
