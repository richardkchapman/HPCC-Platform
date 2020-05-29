# HPCC Local (host) storage

## Directory contents

### hpcc-localfile/

The hpcc-localfile helm chart will provision a new Persistent Volume and a Persistent Volume Claim for each of the required HPCC storage types.
Once installed the generated PVC names should be used when installing the HPCC helm chart.
The values-localfile.yaml is an example of the HPCC storage settings that should be applied, after the "hpcc-localfile" helm chart is installed.
NB: The output of installing this chart, will contain a generated example with the correct PVC names.

Examples of use:

*helm install localfile hpcc-localfile/*
*helm install localfile hpcc-localfile/ --set storage.common.hostpath /home/me/hpcc-data*

### values-localfile.yaml

An example values file to be supplied when installing the HPCC chart.
NB: Either use the output auto-generated when installing the "hpcc-localfile" helm chart, or ensure the names in your values files for the storage types matched the PVC names created.
