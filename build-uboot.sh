#Points to the root of the TI SDK
TI_SDK_PATH=/opt/ti-processor-sdk-linux-am335x-evm-06.03.00.106

LINUX_DEVKIT_PATH=${TI_SDK_PATH}/linux-devkit

#Cross compiler prefix
export CROSS_COMPILE=${LINUX_DEVKIT_PATH}/sysroots/x86_64-arago-linux/usr/bin/arm-linux-gnueabihf-


if [ $1 == "distclean" ];then
make O=am335x distclean
echo "Distclean"

elif [ $1 == "defconfig" ];then
make O=am335x am335x_evm_defconfig
echo "Config am335x_evm"

elif [ $1 == "make" ];then	
make O=am335x
cp -rf am335x/spl/u-boot-spl.bin  /mnt/hgfs/ubuntu
cp -rf am335x/MLO am335x/u-boot.bin am335x/u-boot.img   /mnt/hgfs/ubuntu
echo "Make!!!"

elif [ $1 == "menuconfig" ];then	
make O=am335x menuconfig
echo "Menu Config!!!"

else
echo "No Operation!!!"
fi

#make O=am335x distclean
#make O=am335x am335x_evm_defconfig
#make O=am335x all

