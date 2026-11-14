#! /bin/bash

# Exercise the kubectl transport for "namespace/pod[/container]:path" using a
# stub kubectl that runs the requested command locally.

export TZ=UTC

rm -rf k8s-stub
mkdir -p k8s-stub/tmp
K8S_STUB_DIR="${PWD}/k8s-stub"

cat > k8s-stub/kubectl <<EOF
#! /bin/sh

argv_log="${K8S_STUB_DIR}/argv.log"
prefix=""
while [ \$# -gt 0 ] && [ "\$1" != "--" ]; do
    prefix="\${prefix} \$1"
    case "\$1" in
        no-such-pod)
            echo 'Error from server (NotFound): pods "no-such-pod" not found' >&2
            exit 1
            ;;
    esac
    shift
done
if [ "\$1" != "--" ] || [ "\$2" != "sh" ] || [ "\$3" != "-c" ]; then
    echo "unexpected kubectl invocation:\${prefix} \$*" >&2
    exit 2
fi
echo "kubectl\${prefix}" >> "\${argv_log}"
TMPDIR="${K8S_STUB_DIR}/tmp" exec sh -c "\$4"
EOF
chmod +x k8s-stub/kubectl

mkdir -p k8s-local
echo "Hello, local!" > k8s-local/b:c

run_test ${lnav_test} -d /tmp/lnav.err -n \
    k8s-local/b:c

check_output "a local file that looks like a container path was not read?" <<EOF
Hello, local!
EOF

${lnav_test} -d /tmp/lnav.err -nN \
    -c ":config /tuning/remote/kubectl/command ${K8S_STUB_DIR}/kubectl"

run_test ${lnav_test} -d /tmp/lnav.err -n \
    test-ns/no-such-pod:/var/log/x

check_error_output "no error for a missing pod?" <<EOF
  error: unable to open file: test-ns/no-such-pod:
 reason: unable to open remote path “test-ns/no-such-pod:”
 |        reason: failed to exec in container: connection closed before the tailer was sent -- Error from server (NotFound): pods "no-such-pod" not found
EOF

rm -f k8s-stub/argv.log
run_test ${lnav_test} -d /tmp/lnav.err -n \
    test-ns/web-0:${test_dir}/logfile_access_log.0

check_output "could not read a file in a container?" <<EOF
192.168.202.254 - - [20/Jul/2009:22:59:26 +0000] "GET /vmw/cgi/tramp HTTP/1.0" 200 134 "-" "gPXE/0.9.7"
192.168.202.254 - - [20/Jul/2009:22:59:29 +0000] "GET /vmw/vSphere/default/vmkboot.gz HTTP/1.0" 404 46210 "-" "gPXE/0.9.7"
192.168.202.254 - - [20/Jul/2009:22:59:29 +0000] "GET /vmw/vSphere/default/vmkernel.gz HTTP/1.0" 200 78929 "-" "gPXE/0.9.7"
EOF

run_test sort -u k8s-stub/argv.log

check_output "kubectl was not invoked as expected?" <<EOF
kubectl -n test-ns exec -i -q web-0
EOF

rm -f k8s-stub/argv.log
run_test ${lnav_test} -d /tmp/lnav.err -n \
    test-ns/web-0/app:${test_dir}/logfile_access_log.0

check_output "could not read a file in a specific container?" <<EOF
192.168.202.254 - - [20/Jul/2009:22:59:26 +0000] "GET /vmw/cgi/tramp HTTP/1.0" 200 134 "-" "gPXE/0.9.7"
192.168.202.254 - - [20/Jul/2009:22:59:29 +0000] "GET /vmw/vSphere/default/vmkboot.gz HTTP/1.0" 404 46210 "-" "gPXE/0.9.7"
192.168.202.254 - - [20/Jul/2009:22:59:29 +0000] "GET /vmw/vSphere/default/vmkernel.gz HTTP/1.0" 200 78929 "-" "gPXE/0.9.7"
EOF

run_test sort -u k8s-stub/argv.log

check_output "kubectl did not get the container?" <<EOF
kubectl -n test-ns exec -i -q web-0 -c app
EOF

# Too small to be compressed by the tailer
echo "Hello, container!" > k8s-local/small.txt
run_test ${lnav_test} -d /tmp/lnav.err -n \
    test-ns/web-0:${PWD}/k8s-local/small.txt

check_output "could not read a small file in a container?" <<EOF
Hello, container!
EOF

# Large enough to be compressed by the tailer
seq 1 2000 | sed -e 's/^/line /' > k8s-local/large.txt
LARGE_SUM=$(cksum < k8s-local/large.txt)
run_test sh -c "${lnav_test} -d /tmp/lnav.err -n test-ns/web-0:${PWD}/k8s-local/large.txt | cksum"

check_output "a compressed file in a container was not read correctly?" <<EOF
${LARGE_SUM}
EOF

run_test ls k8s-stub/tmp

check_output "the tailer was left behind in the container?" <<EOF
EOF
