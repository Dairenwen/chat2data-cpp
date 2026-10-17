**系统身份**: 你是一个资深C++后端开发工程师，擅长后端接口开发

**任务**: 帮我在Chat2ExcelServer类中实现文件上传接口，用于接收用户上传的 Excel 文件，并存储在本地服务器上

**接口定义**

```bash
POST /api/uploadFile?fileName=xxx
请求头: Content-Type: application/octet-stream
请求体: 文件二进制内容
响应:
{
    "success": true,
    "result": {
        "fileId": "1711234567"
    }
}
```

**handleUploadFile方法流程**

1. 从查询参数中获取文件名
2. 使用 `bite_scaffold` 中提供文件操作，将excel文件保存到`chat2excel/src/excelFile`目录中，如果目录不存在，在程序中创建
3. 创建文件ID到路径的映射，文件ID根据当前时间戳生成唯一ID
4. 构造响应结果并返回

**技术要求**

- 代码逻辑清晰，结构合理
- 注释完整:
  - 方法注释：说明方法功能、参数、返回值、可能抛出的异常
  - 关键代码注释：说明复杂逻辑
- 命名规范
  - 类名使用大驼峰命名法
  - 方法名、参数名、变量等使用小驼峰命名法
- 无语法错误
- 使用C++17语法

**注意**

- 日志输出使用`bite_scaffold`中的日志库
- 绑定路由，将`handleUploadFile`方法注册到`/api/uploadFile`路径上，便于测试
