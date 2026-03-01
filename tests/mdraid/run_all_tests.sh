for t in /var/tmp/test_[b-k]*.sh; do
    echo "=== $t ==="
    sh $t && echo "STATUS: PASS" || echo "STATUS: FAIL"
    echo ""
done
