#include <stdio.h>
#include <stdlib.h>

struct QNode {
    int key;
    struct QNode* next;
};


struct Queue {
    struct QNode *front, *rear;
};

struct QNode* newNode(int k)
{
    struct QNode* temp
        = (struct QNode*)malloc(sizeof(struct QNode));
    temp->key = k;
    temp->next = NULL;
    return temp;
}

struct Queue* createQueue()
{
    struct Queue* q
        = (struct Queue*)malloc(sizeof(struct Queue));
    q->front = q->rear = NULL;
    return q;
}

void enQueue(struct Queue* q, int k)
{
    struct QNode* temp = newNode(k);

    if (q->rear == NULL) {
        q->front = q->rear = temp;
        return;
    }

    q->rear->next = temp;
    q->rear = temp;

    printf("Enqueued %d\n", k);
}

void deQueue(struct Queue* q)
{
    if (q->front == NULL)
        printf("Cannot dequeue an empty queue.\n");
        return;

    struct QNode* temp = q->front;

    q->front = q->front->next;

    if (q->front == NULL)
        q->rear = NULL;

    free(temp);

    printf("Dequeued %d\n", q);
}

int main()
{
    struct Queue* q = createQueue();
    enQueue(q, 0);
    printf("Queue Front : %d \n", ((q->front != NULL) ? (q->front)->key : -1));
    printf("Queue Rear : %d\n", ((q->rear != NULL) ? (q->rear)->key : -1));
    enQueue(q, 1);
    printf("Queue Front : %d \n", ((q->front != NULL) ? (q->front)->key : -1));
    printf("Queue Rear : %d\n", ((q->rear != NULL) ? (q->rear)->key : -1));
    deQueue(q);
    printf("Queue Front : %d \n", ((q->front != NULL) ? (q->front)->key : -1));
    printf("Queue Rear : %d\n", ((q->rear != NULL) ? (q->rear)->key : -1));
    deQueue(q);
    printf("Queue Front : %d \n", ((q->front != NULL) ? (q->front)->key : -1));
    printf("Queue Rear : %d\n", ((q->rear != NULL) ? (q->rear)->key : -1));
    enQueue(q, 2);
    printf("Queue Front : %d \n", ((q->front != NULL) ? (q->front)->key : -1));
    printf("Queue Rear : %d\n", ((q->rear != NULL) ? (q->rear)->key : -1));
    enQueue(q, 3);
    printf("Queue Front : %d \n", ((q->front != NULL) ? (q->front)->key : -1));
    printf("Queue Rear : %d\n", ((q->rear != NULL) ? (q->rear)->key : -1));
    enQueue(q, 4);
    printf("Queue Front : %d \n", ((q->front != NULL) ? (q->front)->key : -1));
    printf("Queue Rear : %d\n", ((q->rear != NULL) ? (q->rear)->key : -1));
    deQueue(q);
    printf("Queue Front : %d \n", ((q->front != NULL) ? (q->front)->key : -1));
    printf("Queue Rear : %d\n", ((q->rear != NULL) ? (q->rear)->key : -1));
    return 0;
}
