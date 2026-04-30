
source ../base-build-functions.sh
dirname="satsuma"

branchorcommit="2a747913cce0d52fdb131637ff53904b478a0d5e" # updated 2026-01-29
fetch_and_extract $dirname CMakeLists.txt https://github.com/4nng0/satsuma/archive/${branchorcommit}.zip

sed -i 's/-Werror//g' CMakeLists.txt

echo "[$dirname] Building ..."
mkdir -p build
cd build
cmake ..
make
cd ..
echo "[$dirname] Build complete"
mv build/libsatsuma.a . 
