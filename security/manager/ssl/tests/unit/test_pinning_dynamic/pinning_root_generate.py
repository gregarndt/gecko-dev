#!/usr/bin/python

# after runing this file you MUST modify test_pinning_dynamic to change the
# fingerprint of the pinningroot

import tempfile, os, sys
import random
import pexpect
import subprocess
import shutil

libpath = os.path.abspath('../psm_common_py')
sys.path.append(libpath)

import CertUtils

dest_dir = os.getcwd()
db = tempfile.mkdtemp()

CA_basic_constraints = "basicConstraints = critical, CA:TRUE\n"
CA_min_ku = "keyUsage = critical, digitalSignature, keyCertSign, cRLSign\n"
subject_key_ident = "subjectKeyIdentifier = hash\n"

def generate_root_cert(db_dir, dest_dir, prefix, ext_text):
    serial_num = 12223299546
    name = prefix
    key_name = dest_dir + "/" + name + ".key"
    os.system ("openssl genpkey -algorithm RSA -out " + key_name +
                 " -pkeyopt rsa_keygen_bits:2048")

    csr_name =  dest_dir + "/" + name + ".csr"
    os.system ("openssl req -new -key " + key_name + " -days 3650" +
               " -extensions v3_ca -batch -out " + csr_name +
               " -utf8 -subj '/C=US/ST=CA/L=Mountain View" +
               "/O=Mozilla - Pinning test CA/OU=Security Engineering" +
               "/CN=XPCShell Pinning Testing (untrustworthy) CA'")

    extensions_filename = db_dir + "/openssl-exts"
    f = open(extensions_filename, 'w')
    f.write(ext_text)
    f.close()

    cert_name =  dest_dir + "/" + name + ".der"
    signer_key_filename = key_name
    os.system ("openssl x509 -req -sha256 -days 3650 -in " + csr_name +
               " -signkey " + signer_key_filename +
               " -set_serial " + str(serial_num) +
               " -extfile " + extensions_filename +
               " -outform DER -out " + cert_name)

    return key_name, cert_name

prefix = "pinningroot"
[ca_key, ca_cert] = generate_root_cert(db, dest_dir, prefix,
                                       CA_basic_constraints +
                                       CA_min_ku + subject_key_ident)
CertUtils.generate_pkcs12(db, dest_dir, ca_cert, ca_key, prefix)
print ("You now MUST modify test_pinning_dynamic to ensure the xpchell debug " +
       "certificate there matches this newly generated one\n")
