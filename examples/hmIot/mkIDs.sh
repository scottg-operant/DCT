#! /bin/bash
# mkIDs schema - script to create id bundles needed to run an app with some iot mbps schema
#  'schema' is the filename of the schema's .trust file
PATH=../../../tools:$PATH

device=(frontdoor backdoor gate patio)
operator=(alice bob)

if [ -z "$1" ]; then echo "-$0: must supply a .trust schema filename"; exit 1; fi;
if [ ! -r "$1" ]; then echo "-$0: file $1 not readable"; exit 1; fi;

Schema=${1##*/}
Schema=${Schema%.trust}
Bschema=$Schema.scm
RootCert=$Schema.root
SchemaCert=$Schema.schema
KMCapCert=
DeviceSigner=$RootCert

schemaCompile -o $Bschema $1

# extract the info needed to make certs from the compiled schema
Pub=$(schema_info $Bschema);
PubPrefix=$(schema_info $Bschema "#pubPrefix");
PubValidator=$(schema_info -t $Bschema "#pubValidator");

make_cert -s $PubValidator -o $RootCert $PubPrefix
schema_cert -o $SchemaCert $Bschema $RootCert

if [ $(schema_info -t $Bschema "#wireValidator") == AEAD ]; then
    if [ -z $(schema_info -c $Bschema "KM") ]; then
	echo
	echo "- error: AEAD encryption requires entity(s) with a KM (KeyMaker) Capability"
	echo "         but schema $1 doesn't have any."
	exit 1;
    fi;
    # make the 'key maker' capability cert
    KMCapCert=km.cap
    DeviceSigner=$KMCapCert
    make_cert -s $PubValidator -o $KMCapCert $PubPrefix/CAP/KM/1 $RootCert
fi;

# make the device certs
for nm in ${device[@]}; do
    make_cert -s $PubValidator -o $nm.cert $PubPrefix/device/$nm $DeviceSigner
done

# make the operator certs
for nm in ${operator[@]}; do
    make_cert -s $PubValidator -o $nm.cert $PubPrefix/operator/$nm $RootCert
done

# The schema signing certs are signed by the root cert.
# Each identity's bundle consist of the root cert, the schema cert and the
# role cert, in that order. The "+" on the role cert indicates that its
# signing key should be included in the bundle.
# The other certs don't (and shouldn't) have signing keys.

# make the ID bundles. If the schema uses AEAD encryption, devices are
# given "KM (Key Maker) capability but not operators. 
for nm in ${operator[@]}; do
    make_bundle -v -o $nm.bundle $RootCert $SchemaCert +$nm.cert
done

for nm in ${device[@]}; do
    if [ -n $KMCapCert ]; then
	make_bundle -v -o $nm.bundle $RootCert $SchemaCert $KMCapCert +$nm.cert
    else
	make_bundle -v -o $nm.bundle $RootCert $SchemaCert +$nm.cert
    fi
done
