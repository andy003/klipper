# Klipper Development Container

这个开发容器为 Klipper 3D 打印机固件开发提供了完整的开发环境。

## 🚀 快速开始

1. **安装前提条件**:
   - [Docker](https://docs.docker.com/get-docker/)
   - [Visual Studio Code](https://code.visualstudio.com/)
   - [Dev Containers 扩展](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers)

2. **打开项目**:
   - 在 VS Code 中打开这个项目
   - 当提示时选择 "Reopen in Container"
   - 或者使用命令面板: `Dev Containers: Reopen in Container`

3. **等待设置完成**:
   - 容器会自动下载并安装所有依赖
   - 这可能需要几分钟时间

## 🛠️ 包含的工具

### 编译工具链
- **GCC**: 系统编译器
- **GCC-ARM**: ARM Cortex-M 微控制器交叉编译器
- **GCC-AVR**: AVR 微控制器交叉编译器
- **Make**: 构建系统

### Python 环境
- **Python 3.11**: 主要开发语言
- **虚拟环境**: 隔离的 Python 环境 (`klippy-env/`)
- **开发工具**: black, flake8, pylint, mypy

### 硬件工具
- **avrdude**: AVR 编程器
- **stm32flash**: STM32 编程器
- **dfu-util**: DFU 模式编程工具
- **minicom**: 串口终端
- **can-utils**: CAN 总线工具

### VS Code 扩展
- Python 开发支持
- C/C++ 开发支持
- Makefile 支持
- YAML/JSON 编辑器
- Hexdump 查看器

## 📝 使用指南

### 构建固件
```bash
# 配置目标微控制器
make menuconfig

# 编译固件
make

# 清理构建文件
make clean
```

### 运行 Klippy (主机软件)
```bash
# 启动 Klippy
klippy klippy/klippy.py printer.cfg

# 或使用完整路径
~/klippy-env/bin/python klippy/klippy.py printer.cfg
```

### 运行测试
```bash
# 运行 Klippy 测试套件
klipper-test

# 或手动运行
~/klippy-env/bin/python scripts/test_klippy.py
```

### 有用的别名
- `klippy`: 虚拟环境中的 Python
- `klipper-build`: 清理并重新配置构建
- `klipper-flash`: 刷写固件
- `klipper-test`: 运行测试

## 🔌 硬件连接

### 串口访问
容器配置为访问主机的 `/dev` 目录，允许直接连接到串口设备：
- `/dev/ttyUSB0`, `/dev/ttyUSB1`, ... (USB 串口)
- `/dev/ttyACM0`, `/dev/ttyACM1`, ... (Arduino 兼容设备)

### 权限
容器内的用户自动添加到 `dialout` 组，具有串口访问权限。

## 🌐 端口转发

开发容器预配置了以下端口转发：
- **8080**: Mainsail/Fluidd Web 界面
- **7125**: Moonraker API

## 📁 文件结构

```
.devcontainer/
├── devcontainer.json    # Dev Container 配置
├── setup.sh            # 环境设置脚本
└── README.md           # 本说明文档

项目根目录/
├── klippy-env/         # Python 虚拟环境 (自动创建)
├── printer.cfg         # 示例打印机配置 (自动创建)
└── out/               # 构建输出目录
```

## 🔧 自定义配置

### 修改 Python 版本
编辑 `.devcontainer/devcontainer.json` 中的 Python feature:
```json
"ghcr.io/devcontainers/features/python:1": {
    "version": "3.12"
}
```

### 添加额外包
编辑 `.devcontainer/setup.sh` 中的 `PKGLIST` 变量。

### VS Code 设置
在 `.devcontainer/devcontainer.json` 的 `customizations.vscode.settings` 中添加设置。

## 🐛 故障排除

### Python 虚拟环境问题
```bash
# 重新创建虚拟环境
rm -rf klippy-env
python3 -m venv klippy-env
klippy-env/bin/pip install -r scripts/klippy-requirements.txt
```

### 构建错误
```bash
# 清理并重新配置
make clean
make menuconfig
```

### 串口权限问题
```bash
# 检查用户组
groups

# 确保在 dialout 组中
sudo usermod -a -G dialout $USER
```

## 📚 相关资源

- [Klipper 官方文档](https://www.klipper3d.org/)
- [Klipper GitHub 仓库](https://github.com/Klipper3d/klipper)
- [配置参考](https://www.klipper3d.org/Config_Reference.html)
- [安装指南](https://www.klipper3d.org/Installation.html)