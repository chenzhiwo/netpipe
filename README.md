# netpipe
* 一个通过网络远程控制的管道小程序
netpipe可以通过远程命令启动两个程序in和out，同时把out的stdout连接到in的stdin

# 安装
安装直接就可以
`` make ``
然后随意使用netpipe

# 使用

### 协议
程序默认使用30000端口
当启动netpipe以后，可以使用nc来连接到控制端
`` nc IP_ADDR 30000 ``
连接成功以后netpipe就会输出版本信息
询问要启动的程序，可输入in/out
```
which prog?
in
```
询问程序的参数
```
what args?
nc -l 1234
```
