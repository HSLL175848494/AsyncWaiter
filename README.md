# HSAsyncWaiter 异步等待器

## 概述

基于模板的C++异步等待器，提供了一种灵活的方式来管理和协调异步操作。它允许在不同的线程之间通过键值对的方式进行值传递和事件通知，支持超时管理、自动清理。

## 基本用法

### 头文件包含

```cpp
#include "HSAsyncWaiter.h"
using namespace HSLL;
```

### 1. 创建异步等待器

```cpp
// 使用int作为键类型
HSAsyncWaiter<int> waiter;

// 启动清理线程
waiter.Start(50);  // 50ms清理间隔
```

### 2. 创建异步值

```cpp
// 创建带返回值的异步事件
waiter.Create<int>(123, false, 5000);  // 键=123，不替换已存在的值，5秒后删除事件

// 创建无值事件
waiter.Create(456, true, 3000);  // 键=456，替换已存在的值，3秒后删除事件
```

### 3. 设置异步值

```cpp
// 任意线程设置带返回值的异步值
waiter.SetValue<int>(123, 100);  // 为键123设置值100

// 任意线程设置事件
waiter.Set(456);  // 触发键456的事件
```

### 4. 等待异步值

```cpp
// 任意线程等待带返回值的异步值
int result;
if (waiter.WaitValue<int>(123, result, true, 1000)) {
    // 成功获取值，result = 100
    // 第三个参数true表示获取后自动删除异步值
    // 第四个参数1000表示等待超时1秒
}

// 任意线程等待事件
if (waiter.Wait(456, true, 2000)) {
    // 事件已发生
}
```

### 5. 删除异步值

```cpp
// 删除指定键的异步值
waiter.Remove(123);
```

### 6. 停止异步等待器

```cpp
// 停止清理线程并清理所有异步值
waiter.Stop();
```

## 支持的键类型

支持任何可以作为`std::map`键的类型，包括：

- 基本类型：int, long, std::string等
- 自定义类型：需要实现比较运算符或提供比较函数对象
