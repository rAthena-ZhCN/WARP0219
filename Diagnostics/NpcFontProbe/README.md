# NpcFontProbe 诊断补丁

## 用法
1. 把 NpcFontProbe.dll 放到客户端根目录（和 Ragexe 同一个目录）。
2. 打开 WARP0219。
3. 勾选 `Custom DLL` 补丁。
4. 文件选择 `Inputs/DLLSpec_NpcFontProbe.yml`。
5. 应用补丁并启动客户端。
6. 进游戏后打开任意 NPC 对话和选项。
7. 退出客户端后，客户端目录会生成 `NpcFontProbe.log`。

把 NpcFontProbe.log 发回来即可定位 NPC 对话和选项使用的字体创建/绘制调用点。

## 日志内容
- FONTA / FONTI：CreateFontA / CreateFontIndirectA 的返回地址、高度、字符集、字体名
- TEXTOUT / EXTTEXTOUT / DRAWTEXT：文字绘制调用的返回地址、字体句柄、坐标和文本样本

日志只读诊断，不修改客户端行为。
