# Chat2Data 错误码与异常类设计

## 一、系统身份

你是一个资深的 C++ 后端开发工程师，在项目开发时需要使用自定义异常处理项目中异常以及错误情况。

## 二、任务

为 chat2Data 项目实现错误码、错误码枚举类 `ErrorCode` 定义以及 `Chat2DataException` 异常类定义。

## 三、错误码说明

### 1. 错误码定义

- 错误码放在 `ErrorCode` 枚举类中，`ErrorCode` 枚举使用 C++11 标准定义，枚举值为整数类型。
- `SUCCESS` 为通用成功码，枚举值是 `0`。
- 用户子服务错误码范围 `100~199`，错误码格式：`USER_xxx_xxx`，比如：`USER_NICKNAME_EXISTS` 用户昵称已存在。
- 文件子服务错误码范围 `200~299`，错误码格式：`FILE_xxx_xxx`，比如：`FILE_NOT_FOUND` 文件不存在。
- 数据库子服务错误码范围 `300~399`，错误码格式：`DB_xxx_xxx`，比如：`DB_CONNECTION_FAILED` 数据库连接失败。
- Excel 解析子服务错误码范围 `400~499`，错误码格式：`EXCEL_xxx_xxx`，比如：`EXCEL_PARSE_FAILED` Excel 解析失败。
- 通知子服务错误码范围 `500~599`，错误码格式：`NOTIFY_xxx_xxx`，比如：`NOTIFY_SEND_FAILED` 通知发送失败。
- AI 子服务错误码范围 `600~699`，错误码格式：`AI_xxx_xxx`，比如：`AI_MODEL_NOT_FOUND` AI 模型不存在。
- 网关子服务错误码范围 `700~799`，错误码格式：`GATEWAY_xxx_xxx`，比如：`GATEWAY_CONNECTION_FAILED` 网关连接失败。

### 2. 注意事项

1. 在 `ErrorCode` 枚举类中只定义一个通用成功码 `SUCCESS`，枚举值是 `0`。
2. 其余子服务的错误码，将在后续项目实现时根据实际情况进行添加，但每个子服务错误码范围注释需要添加。
3. 类要包含在 `chat2Data` 命名空间中。

### 3. 全局方法 error2String

添加全局方法 `error2String`，接收一个错误码，返回错误码对应的错误描述符。

要求：

1. 实现时采用 `unordered_map` 建立错误码和描述符映射。
2. 映射关系举例如下：

   ```cpp
   {ErrorCode::SUCCESS, "操作成功"},
   {ErrorCode::USER_NICKNAME_EXISTS, "用户昵称已存在"},
   ```
3. 每个子服务具体的错误描述符待后续添加，需要添加各个子服务错误码描述符注释区分。
4. 如果错误码存在，返回对应的错误描述符，否则返回 `"未知错误"`。

### 4. 全局方法 getServiceName

添加全局方法 `getServiceName`，接收一个错误码，返回错误码所属的子服务名称。

要求：

1. 网关子服务名称：`gatewayService`
2. 用户子服务名称：`userService`
3. 文件子服务名称：`fileService`
4. 数据库子服务名称：`dbService`
5. Excel 解析子服务名称：`excelService`
6. 通知子服务名称：`notifyService`
7. AI 子服务名称：`aiService`
8. 验证码不存在时，返回 `"未知服务"`。

## 四、Chat2DataException 说明

1. `Chat2DataException` 异常类继承自 `std::exception` 类，用于处理 chat2Data 项目中发生的异常情况。
2. `Chat2DataException` 异常类中，添加构造函数，接收一个错误码。
3. `Chat2DataException` 异常类中，重写 `what()` 方法，返回错误描述符的指针，格式要求：`"服务名称: 错误描述符"`。
4. `Chat2DataException` 异常类中，添加一个成员变量，用于存储错误码和错误描述符，要求：
   - 错误码：`ErrorCode` 枚举类中的值。
   - 错误描述符：`error2String` 方法返回的错误描述符字符串。

## 五、输出要求

### 1. 头文件：errorHandler.hpp

- 存放枚举类 `ErrorCode` 定义。
- 存放全局方法 `error2String`、`getServiceName` 的声明。
- 存放 `Chat2DataException` 异常类的声明。

### 2. 源文件：errorHandler.cpp

- 实现全局方法 `error2String`、`getServiceName`。
- 实现 `Chat2DataException` 异常类。

### 3. 代码存放路径

将代码文件放置在以下目录：

`chat2data-cpp/chat2Data/common`

---

先阅读**AGENT.md**，再严格按照上述要求实现。
