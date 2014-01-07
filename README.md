# TokyoTyrant IPLIST Skeleton Plugin

## Introduction

The is a skeleton plugin of `TokyoTyrant` server. It stores IPv4 addresses in memroy.
If the given IP address is in memory, the `get` request will get a "1" response, otherwise "0".
The IP addresses can be set using `put` request and can be removed using `out` request. 
The key can be form of IP address range, such as "192.168.1.100-192.168.1.200", or CIDR, such as "192.168.1.0/24".

## Build

```shell
    cd ttskeliplist
    make
```

## Use

```shell
    ttserver -skel ttskeliplist.so
````

## Author

FengGu <flygoast@126.com>
