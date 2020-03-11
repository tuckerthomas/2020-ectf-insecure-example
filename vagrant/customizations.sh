# add tools to the path
# THIS MUST BE UPDATED TO WORK WITH YOUR DESIGN SO THAT ALL TOOLS ARE ON THE PATH
export PATH="/ectf/tools:$PATH"
echo "export PATH=/ectf/tools:$PATH" >> ~/.bashrc

# Install Python 3 Package Manager
sudo apt install -y python3-pip

# Install PyNacl Library
sudo pip3 install pynacl

# Built BearSSL Library
cd /ectf/BearSSL && make CONF=Microblaze