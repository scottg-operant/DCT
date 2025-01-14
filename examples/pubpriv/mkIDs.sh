#! /bin/bash
# mkIDs schema - script to create id bundles needed to run location reporter example
#  'schema' is the filename of the schema's .trust file
PATH=../../../tools:$PATH

numLR=9
numMon=2

if [ -z "$1" ]; then echo "-$0: must supply a .trust schema filename"; exit 1; fi;
if [ ! -r "$1" ]; then echo "-$0: file $1 not readable"; exit 1; fi;

Schema=${1##*/}
Schema=${Schema%.trust}
Bschema=$Schema.scm
RootCert=$Schema.root
SchemaCert=$Schema.schema
SGCapCert=

schemaCompile -o $Bschema $1

# extract the info needed to make certs from the compiled schema
Pub=$(schema_info $Bschema);
PubPrefix=$(schema_info $Bschema "#pubPrefix");
PubValidator=$(schema_info -t $Bschema "#pubValidator");

make_cert -s $PubValidator -o $RootCert $PubPrefix
schema_cert -o $SchemaCert $Bschema $RootCert

# PPAEAD and PPSIGN require a subscription group capability cert.
# At least one member of the SG must have a non-zero capability argument
# in this cert so that there is at least on key maker
# Here, just setting all SG members to be potential key makers.
if [ -z $(schema_info -c $Bschema "SG") ]; then
    echo
    echo "- error: PPAEAD/PPSIGN require entity(s) with a SG (subscription group) Capability"
    echo "         but schema $1 doesn't have any."
    exit 1;
fi;
# make the 'subscriber group' capability cert (with ability to be a key maker)
SGCapCert=sg.cap
make_cert -s $PubValidator -o $SGCapCert $PubPrefix/CAP/SG/1 $RootCert

# make the location reporter certs
for (( n=1; n <= $numLR; ++n )); do
    make_cert -s $PubValidator -o locR$n.cert $PubPrefix/locRptr/$n $RootCert
done

# make the monitor certs
# if there's a subscriber group, putting all the monitors in it
for (( n=1; n <= $numMon; ++n )); do
    if [ -n $SGCapCert ]; then
        make_cert -s $PubValidator -o mon$n.cert $PubPrefix/monitor/$n $SGCapCert
    else
        make_cert -s $PubValidator -o mon$n.cert $PubPrefix/monitor/$n $RootCert
    fi;
done

# The schema signing certs are signed by the root cert.
# Each identity's bundle consist of the root cert, the schema cert and the
# role cert, in that order. The "+" on the role cert indicates that its
# signing key should be included in the bundle.
# The other certs don't (and shouldn't) have signing keys.

# make the ID bundles. If the schema uses PPAEAD encryption, only monitors
# given "SG" (subscriber group) capability 
for (( n=1; n <= $numLR; ++n )); do
    make_bundle -v -o locRptr$n.bundle $RootCert $SchemaCert +locR$n.cert
done

for (( n=1; n <= $numMon; ++n )); do
    if [ -n $SGCapCert ]; then
	    make_bundle -v -o monitor$n.bundle $RootCert $SchemaCert $SGCapCert +mon$n.cert
    else
	    make_bundle -v -o monitor$n.bundle $RootCert $SchemaCert +mon$n.cert
    fi;
done
