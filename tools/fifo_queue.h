
#ifndef __FIFO_QUEUE_H__
#define __FIFO_QUEUE_H__

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>
#include "../hash_hot_cmt/dftl_types.h"

typedef struct prefill_type QDataType;

typedef struct QueueNode // 节点的结构体
{
    QDataType val;
    struct QueueNode *next;
} QNode;

typedef struct Queue
{
    QNode *phead;
    QNode *ptail;
    int size;
    int max_size; // 新增：队列的最大容量
} Queue;
void QInit(Queue *q, int maxSize); // 初始化
void QDestroy(Queue *q);           // 销毁

void QPush(Queue *q, QDataType x); // 插入
void QPop(Queue *q);               // 删除

QNode QBack(Queue *q);  // 返回最后一个节点数据
QNode QFront(Queue *q); // 返回第一个节点数据

bool QEmpty(Queue *q); // 是否为空

int QSize(Queue *q); // 元素数量
#endif