#!/usr/bin/env python3

#Used to load json files
from json import load
#used to parse arguments in the command line
from argparse import ArgumentParser

# Used for decryption

import nacl.bindings as b
import nacl.exceptions as exc
import nacl.hash
import nacl.encoding

# calculating the chunk remainder
from math import floor

# TODO: Move decryption processes to separate functions
# TODO: Add argument parsing

def decrypt_song(keys_loc, infile, outfile):
    """Description decrypts song 
    Args:
        keys_loc: location to json keys file
        infile: file path to the encrypted song
        outfile: file path to output decrypted song
    Returns:
        Outputs the decrypted version of the encrypted song

    """
    # Configuration Variables
    mac_size = 16           # poly1305 mac size
    hash_byte_size = 12     # Take 12 bytes of a 256 bit hash
    aad_size = 4            # Use 4 bytes to store aad size
    wave_header_size = 44
    metadata_size_allocation = 4
    encrypted_wave_header_size = wave_header_size + metadata_size_allocation + mac_size
    chunk_size = 16000
    
    print("Setting chunksize to " + str(chunk_size) + " bytes")

    #opens decrypted and encrypted song locations
    encrypted_song = open(infile, 'rb')    
    decrypted_song = open(outfile, 'wb')   

    #opens json keys file
    keys_file = load(open(keys_loc, "r"))

    print("key " + keys_file["key"])

    #key to decrypt the encrypted song file
    key = bytes.fromhex(keys_file["key"])

    print("Starting decrypt song")

    #nonce to decrypt song
    nonce = encrypted_song.read(hash_byte_size)
    print("Wave Header Nonce: " + str(nonce))

    #read encrypted file header
    encrypted_wave_header = encrypted_song.read(encrypted_wave_header_size)

    aad = b"wave_header\0"

    # Encrypt Wav Header
    wav_header = b.crypto_aead_chacha20poly1305_ietf_decrypt(encrypted_wave_header, aad, nonce, key)

    #metadata song
    metadata_size = int.from_bytes(wav_header[-metadata_size_allocation:], 'little')
    print("Metadata size: " + str(metadata_size))

    # Strip metadata size off wave_header
    wav_header = wav_header[:wave_header_size]

    #writes out decrypted song header
    decrypted_song.write(wav_header)

    #song size
    song_info_size = int.from_bytes(wav_header[-4:], byteorder='little')
    print("Song size: " + str(song_info_size))

    # Calculate # of chunks to read
    chunk_to_read = floor(song_info_size / chunk_size)
    print("Chunks to read: " + str(chunk_to_read))

    # Calculate remainder
    chunk_remainder = song_info_size % chunk_size
    print("Chunk remainder size: " + str(chunk_remainder))

    #nonce to decrypt metadata
    nonce = encrypted_song.read(hash_byte_size)
    print("Metadata nonce: " + str(nonce))

    aad = b"meta_data\0"

    #encrypted metadata
    encrypted_metadata_tag = encrypted_song.read(mac_size)
    print("Metadata tag: " + str(encrypted_metadata_tag) + " Size: " + str(len(encrypted_metadata_tag)))
    encrypted_metadata = encrypted_song.read(metadata_size)
    print("Encrypted metadata: " + str(encrypted_metadata) + " Size: " + str(len(encrypted_metadata)))

    #encrypted meta data and encrypted meta data tag
    encrypted_metadata_combined = encrypted_metadata + encrypted_metadata_tag

    print("Encrypted data: " + str(encrypted_metadata_combined))
    metadata = b.crypto_aead_chacha20poly1305_ietf_decrypt(encrypted_metadata_combined, aad, nonce, key)

    print("Decrypted metadata: " + str(metadata))
    
    # Get the sha256 sum from the metadata to use in aad
    sha256sum = metadata[:32]
    print("Sha256sum " + str(sha256sum))

    #reads individual chunks
    for i in range(1, chunk_to_read + 1):
        #print("Read chunk: " + str(i))
        nonce = encrypted_song.read(hash_byte_size)
        #print("Chunk " + str(i) + " Nonce: " + str(nonce))
        aad = sha256sum
        encrypted_chunk_tag = encrypted_song.read(mac_size)
        encrypted_chunk_wo_tag = encrypted_song.read(chunk_size)

        #encrypted chunk
        encrypted_chunk = encrypted_chunk_wo_tag + encrypted_chunk_tag

        #decrypted song chunk
        song_chunk = b.crypto_aead_chacha20poly1305_ietf_decrypt(encrypted_chunk, aad, nonce, key)

        #writes out decrypted version of the song
        decrypted_song.write(song_chunk)

        if i == 1:
            print("Read chunk: " + str(i))
            print("Chunk " + str(i) + " Nonce: " + str(nonce))
            print("Chunk " + str(i) + " Tag: " + str(encrypted_chunk_tag))
            print("Chunk " + str(i) + " Decrypted Chunk :" + str(song_chunk))

    #nonce to decrypt remainder
    nonce = encrypted_song.read(hash_byte_size)
    print("Remainder Nonce: " + str(nonce))
    aad = sha256sum

    encrypted_chunk_tag = encrypted_song.read(mac_size)
    encrypted_chunk_wo_tag = encrypted_song.read(chunk_remainder)

    encrypted_chunk = encrypted_chunk_wo_tag + encrypted_chunk_tag

    #decrypted song chunk remainder
    song_chunk = b.crypto_aead_chacha20poly1305_ietf_decrypt(encrypted_chunk, aad, nonce, key)

    #writes out decrypted shunk remainder
    decrypted_song.write(song_chunk)

    #closes encrypted and decrypted song
    encrypted_song.close()
    decrypted_song.close()

    print("Decryption Success")
    

def main():
    """Main function
    Description:
        Parses Arguments sent through the terminal, and initiates the song decryption process

    Returns:
        none
    """
    parser = ArgumentParser(description='main interface to decrytp song')
    parser.add_argument('--outfile', help='path to save the protected song', required=True)
    parser.add_argument('--infile', help='path to unprotected song', required=True)
    args = parser.parse_args()
    decrypt_song("keys.json", args.infile, args.outfile)

#initites main()
if __name__ == '__main__':
    main()
