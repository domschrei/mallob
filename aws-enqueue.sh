#!/usr/bin/env bash

set -x

if [ $# -lt 5 ] ; then
    echo "USAGE: "
    echo "$0 PROFILE PROJECT_NAME COMP_S3_PROBLEM_PATH NUM_PROCESSES JOB_NAME_SUFFIX COMP_OPTIONS "
    echo "where: "
    echo "   PROFILE is a AWS CLI profile with administrator access to the account"
    echo "   PROJECT_NAME is the name of the project.  MUST BE ALL LOWERCASE. Regular expression for names is: "
    echo "       (?:[a-z0-9]+(?:[._-][a-z0-9]+)*/)*[a-z0-9]+(?:[._-][a-z0-9]+)*"
    echo "   COMP_S3_PROBLEM_PATH is the path to the .zip file containing the problem to be analyzed"
    echo "   NUM_PROCESSES is the number of processes per node"
    echo "   JOB_NAME_SUFFIX Some suffix to identify this particular run. Should be different each time you run this"
    echo "   COMP_OPTIONS is an optional parameter variable for any other parameters you want to pass to your solver"
    exit 1
fi

aws --profile $1 batch submit-job --job-name $2$5 --job-queue JobQueue-$2 \
 --job-definition JobDefinition-$2 --node-overrides \
"
{
   \"nodePropertyOverrides\":[
      {
        \"targetNodes\": \"0:1\",

        \"containerOverrides\":{
            \"environment\":[
              {
                  \"name\":\"COMP_S3_PROBLEM_PATH\",
                  \"value\": \"shared-entries/$3\"
              },
              {
                  \"name\":\"COMP_S3_RESULT_PATH\",
                  \"value\": \"results/$3.log\"
              },
              {
                  \"name\":\"COMP_OPTIONS\",
                  \"value\": \"$6\"
              },
              {
                  \"name\":\"NUM_PROCESSES\",
                  \"value\": \"$4\"
              },
              {
                  \"name\":\"S3_BKT\",
                  \"value\":\"sat-comp-2020\"
              }
            ]
          }
        }
      ]
}
"
