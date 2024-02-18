# gtp5g - 5G compatible GTP kernel module
gtp5g is a customized Linux kernel module gtp5g to handle packet by PFCP IEs such as PDR and FAR.
For detailed information, please reference to 3GPP specification TS 29.281 and TS 29.244.

## Notice
Due to the evolution of Linux kernel, this module would not work with every kernel version.
Please run this module with kernel version `5.0.0-23-generic`, upper than `5.4` (Ubuntu 20.04) or RHEL8.

## Usage
### Compile
```
git clone https://github.com/free5gc/gtp5g.git && cd gtp5g
make clean && make
```

### Install kernel module
Install the module to the system and load automatically at boot
```
sudo make install
```

### Remove kernel module
Remove the kernel module from the system
```
sudo make uninstall
```
### Checkout Rules
Get PDR/FAR/QER information by "/proc"
```
# if UPF used legacy netlink struct without SEID, need use #SEID=0 to query related info as below:
echo #interfaceName #SEID #PDRID > /proc/gtp5g/pdr
echo #interfaceName #SEID #FARID > /proc/gtp5g/far
echo #interfaceName #SEID #QERID > /proc/gtp5g/qer
```
```
cat /proc/gtp5g/pdr
cat /proc/gtp5g/far
cat /proc/gtp5g/qer
```

### QoS Enable
Support Session AMBR and MFBR

1) Check whether QoS is enabled or disabled.(1 enabled, 0 disabled)
    ```
    cat /proc/gtp5g/qos
    ```
2) Enable or disable QoS
   + enable
        ```
        echo 1 >  /proc/gtp5g/qos
        ```
   + disable
        ```
        echo 0 >  /proc/gtp5g/qos
        ```

### Log Related
1) check log
    ```
    dmesg
    ```
1) update log level
    ```
    # [number] is 0~4 
    # e.g. echo 4 > /proc/gtp5g/dbg
    echo [number] > /proc/gtp5g/dbg
    ```
### Tools
+ [go-gtp5gnl](https://github.com/free5gc/go-gtp5gnl) usage
    +  Build tool
        ```
        # change directory
        cd ./cmd/gogtp5g-tunnel

        # build gogtp5g-tunnel
        go build
        ```
    +  List all PDR/FAR/QER
        ```
        # ./gtp5g-tunnel list [pdr/far/qer]
        ./gtp5g-tunnel list pdr
        ```
    +  Get/Del/Add/Mod PDR/FAR/QER
        ```
        # ./gtp5g-tunnel [get/del/add/mod] [PDR/FAR/QER] [interface_name] [seid] [id] [option]
        ./gtp5g-tunnel add pdr upfgtp0 1 3 --pcd 99
        ```
        ```
        PDR OPTIONS

                --pcd <precedence>

                --hdr-rm <outer-header-removal>

                --far-id <existed-far-id>

                --ue-ipv4 <pdi-ue-ipv4>

                --f-teid <i-teid> <local-gtpu-ipv4>

                --sdf-desp <description-string>

                --sdf-tos-traff-cls <tos-traffic-class>

                --sdf-scy-param-idx <security-param-idx>

                --sdf-flow-label <flow-label>

                --sdf-id <id>

                --qer-id <id>

        FAR OPTIONS

                --action <apply-action>

                --hdr-creation <description> <o-teid> <peer-ipv4> <peer-port>

        QER OPTIONS

                --qer-id <qer-id>

                --qfi-id <qfi-id> [Value range: {0..63}]

                --rqi-d <rqi> [Value range: {0=not triggered, 1=triggered}]

                --ppp <ppp> [Value range: {0=not present, 1=present}]

                --ppi <ppi> [Value range: {0..7}]
        ```


