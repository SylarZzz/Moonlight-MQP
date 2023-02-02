
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "Queue.h"
#include "Limelight-internal.h"
#include "Limelight.h"

Queue *createQueue() {
    Queue *queue = (Queue*) malloc(sizeof(Queue));
    queue->head = queue->tail = NULL;
    queue->size = 0;
    return queue;
}

bool isEmpty(Queue *queue) {
    return queue->size == 0;
}

void enqueue(Queue *queue, void * data) {
    PQUEUED_DECODE_UNIT qdu = data;

    Node *temp = (Node*) malloc(sizeof(Node));
    temp->data = data;
    temp->next = NULL;

    if (queue->tail == NULL) {
        queue->head = queue->tail = temp;
    } else {
        queue->tail->next = temp;
        queue->tail = temp;
    }
    queue->size++;
    Limelog("Enqueued frame %d\n", data);
}

void dequeue(Queue *queue) {
    if (isEmpty(queue)) {
        Limelog("Cannot dequeue empty queue.\n");
    } else {
        Node *temp = queue->head;
        PQUEUED_DECODE_UNIT qdu = temp->data;
        Limelog("Dequeued frame %d\n", temp->data);
        queue->head = queue->head->next;
        if (queue->head == NULL) {
            queue->tail = NULL;
        }
        free(temp);
        queue->size--;
    }
}

int front(Queue *queue) {
    if (isEmpty(queue)) {
        Limelog("Queue is empty.\n");
        return -1;
    } else {
        return queue->head->data;
    }
}

int back(Queue *queue) {
    if (isEmpty(queue)) {
        Limelog("Queue is empty.\n");
        return -1;
    } else {
        return queue->tail->data;
    }
}

/*
void queueTest() {
    Queue *q = createQueue();
    enqueue(q, 1);
    enqueue(q, 2);
    enqueue(q, 3);
    //printf("The front element is %d\n", front(q));
    dequeue(q);
    enqueue(q, 4);
    printf("The queue size is %d\n", q->size);
    dequeue(q);
    dequeue(q);
    dequeue(q);
    printf("isEmpty? = %d\n", isEmpty(q));
    dequeue(q);
    printf("size = %d\n", q->size);
    free(q);
}
*/
