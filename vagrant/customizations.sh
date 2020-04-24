#!/bin/bash

#------------------------------------------------------------#
# customizations.sh
#
# Summary: Make projects specific changes to the development
# environment for building C code and running Python scripts
#
# NOTE: This file is called by the project's Vagrantfile and 
# run as root
#------------------------------------------------------------#

# Add project tools directory to the PATH variable
export PATH="/ectf/tools:$PATH"
echo "export PATH=/ectf/tools:$PATH" >> ~/.bashrc

# Expand partition to take advantage of all usable space
sed -e 's/\s*\([\+0-9a-zA-Z]*\).*/\1/' << EOF | fdisk /dev/sda
  d # Delete partition 3
  3 #
  n # Make new partition 3
  p #
  3 #
    # Select default start sector
    # Select default end sector
  n # Do not delete existing EXT4 filesystem
  w # Write changes
EOF

# Rediscover new partitions
partprobe

# Resize partion 3's filesystem to fill the available space
sudo resize2fs /dev/sda3

# Install Python 3 Package Manager
sudo apt install -y python3-pip

# Install PyNacl Library
sudo pip3 install pynacl

# Built BearSSL Library
cd /ectf/BearSSL && make CONF=Microblaze
