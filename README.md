```cpp
// 使用字符串作为键  
CAsyncWaiter<std::string> stringWaiter;

// 创建事件
waiter.Create<ResultType>("key1",true, 5000);

//线程1设置值值
waiter.SetValue(strKey, ResultType());

//线程2等待值
ResultType stResult;
bool bRetWait = waiter.WaitValue("key1", stResult);
```
