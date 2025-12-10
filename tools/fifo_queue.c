#include "fifo_queue.h"
void QInit(Queue *q, int maxSize) // 增加 maxSize 参数
{
    assert(q);
    assert(maxSize > 0);

    q->phead = q->ptail = NULL;
    q->size = 0;
    q->max_size = maxSize; // 初始化最大容量
}

// fifo_queue.c

void QPush(Queue *q, QDataType x)
{
    assert(q);
    QNode *newnode = (QNode *)malloc(sizeof(QNode));
    assert(newnode);
    newnode->val = x;
    newnode->next = NULL;

    if (q->phead == NULL)
    {
        q->phead = q->ptail = newnode;
    }
    else
    {
        q->ptail->next = newnode;
        q->ptail = newnode;
    }
    q->size++;
    if (q->size > q->max_size)
    {
        QPop(q);
    }
}

void QPop(Queue *q)
{
    assert(q);
    assert(q->size > 0);
    QNode *next = q->phead->next;
    free(q->phead);
    q->phead = next;
    // 当只有一个节点时：把	q->ptail = NULL;
    if (q->phead == NULL)
    {
        q->ptail = NULL;
    }
    q->size--;
}

QNode QBack(Queue *q)
{
    assert(q);
    assert(q->ptail);
    return *(q->ptail);
}

QNode QFront(Queue *q)
{
    assert(q);
    assert(q->phead);
    return *(q->phead);
}

bool QEmpty(Queue *q)
{
    assert(q);
    return q->size == 0;
}

int QSize(Queue *q)
{
    assert(q);
    return q->size;
}

void QDestroy(Queue *q)
{
    QNode *cur = q->phead;
    while (cur)
    {
        QNode *next = cur->next;
        free(cur);
        cur = next;
    }
    q->phead = q->ptail = NULL;
    q->size = 0;
}
