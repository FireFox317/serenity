mkdir -p Build/aarch64
touch Build/aarch64/_disk_image
export SERENITY_ARCH=aarch64
export SERENITY_DISK_SIZE_BYTES=500000000
./Meta/serenity.sh run
<ctrl-c>
cd Build/aarch64
cmake .
ninja Kernel
ninja Userland/DynamicLoader/Loader.so
ninja Userland/DynamicLoader/install
ninja Userland/Services/SystemServer/SystemServer
ninja Userland/Services/SystemServer/install

ninja Userland/Shell/Shell
ninja Userland/Shell/install
ninja Userland/Libraries/LibLine/libline.so
ninja Userland/Libraries/LibLine/install
ninja Userland/Libraries/LibSyntax/install
ninja Userland/Libraries/LibSyntax/libsyntax.so
ninja Userland/Libraries/LibSyntax/install
ninja Userland/Libraries/LibRegex/libregex.so
ninja Userland/Libraries/LibRegex/install

ninja Kernel
../../Meta/run.sh
