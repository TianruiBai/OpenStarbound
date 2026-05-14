#!/bin/sh -e

mkdir client_distribution
mkdir client_distribution/assets
mkdir client_distribution/assets/user

./dist/asset_packer -c scripts/packing.config assets/opensb client_distribution/assets/opensb.pak

mkdir client_distribution/mods
touch client_distribution/mods/mods_go_here

mkdir client_distribution/osx
cp -LR scripts/ci/macos/Starbound.app client_distribution/osx/
mkdir client_distribution/osx/Starbound.app/Contents/MacOS
cp dist/starbound client_distribution/osx/Starbound.app/Contents/MacOS/
cp dist/*.dylib client_distribution/osx/Starbound.app/Contents/MacOS/
cp \
  dist/starbound_server \
  dist/btree_repacker \
  dist/asset_packer \
  dist/asset_unpacker \
  dist/dump_versioned_json \
  dist/make_versioned_json \
  scripts/ci/macos/sbinit.config \
  scripts/ci/macos/run-server.sh \
  scripts/steam_appid.txt \
  client_distribution/osx/

mkdir server_distribution
mkdir server_distribution/assets

mkdir server_distribution/mods
touch server_distribution/mods/mods_go_here

./dist/asset_packer -c scripts/packing.config -s assets/opensb server_distribution/assets/opensb.pak

mkdir server_distribution/osx
cp \
  dist/starbound_server \
  dist/btree_repacker \
  dist/*.dylib \
  scripts/ci/macos/sbinit.config \
  scripts/ci/macos/run-server.sh \
  scripts/steam_appid.txt \
  server_distribution/osx/

tar -cvf client.tar client_distribution
tar -cvf server.tar server_distribution
