#!/bin/sh

test_description='git maintenance builtin'

GIT_TEST_COMMIT_GRAPH=0
GIT_TEST_MULTI_PACK_INDEX=0

. ./test-lib.sh

test_expect_success 'help text' '
	test_must_fail git maintenance -h 2>err &&
	test_i18ngrep "usage: git maintenance run" err
'

test_expect_success 'run [--auto|--quiet]' '
	GIT_TRACE2_EVENT="$(pwd)/run-no-auto.txt" git maintenance run --no-quiet &&
	GIT_TRACE2_EVENT="$(pwd)/run-auto.txt" git maintenance run --auto &&
	GIT_TRACE2_EVENT="$(pwd)/run-quiet.txt" git maintenance run --quiet &&
	grep ",\"gc\"]" run-no-auto.txt  &&
	! grep ",\"gc\",\"--auto\"" run-auto.txt &&
	grep ",\"gc\",\"--quiet\"" run-quiet.txt
'

test_expect_success 'maintenance.<task>.enabled' '
	git config maintenance.gc.enabled false &&
	git config maintenance.commit-graph.enabled true &&
	git config maintenance.loose-objects.enabled true &&
	GIT_TRACE2_EVENT="$(pwd)/run-config.txt" git maintenance run &&
	! grep ",\"fetch\"" run-config.txt &&
	! grep ",\"gc\"" run-config.txt &&
	! grep ",\"multi-pack-index\"" run-config.txt &&
	grep ",\"commit-graph\"" run-config.txt &&
	grep ",\"prune-packed\"" run-config.txt
'

test_expect_success 'run --task=<task>' '
	GIT_TRACE2_EVENT="$(pwd)/run-commit-graph.txt" git maintenance run --task=commit-graph &&
	GIT_TRACE2_EVENT="$(pwd)/run-gc.txt" git maintenance run --task=gc &&
	GIT_TRACE2_EVENT="$(pwd)/run-commit-graph.txt" git maintenance run --task=commit-graph &&
	GIT_TRACE2_EVENT="$(pwd)/run-both.txt" git maintenance run --task=commit-graph --task=gc &&
	! grep ",\"gc\"" run-commit-graph.txt  &&
	grep ",\"gc\"" run-gc.txt  &&
	grep ",\"gc\"" run-both.txt  &&
	grep ",\"commit-graph\",\"write\"" run-commit-graph.txt  &&
	! grep ",\"commit-graph\",\"write\"" run-gc.txt  &&
	grep ",\"commit-graph\",\"write\"" run-both.txt
'

test_expect_success 'run --task=bogus' '
	test_must_fail git maintenance run --task=bogus 2>err &&
	test_i18ngrep "is not a valid task" err
'

test_expect_success 'run --task duplicate' '
	test_must_fail git maintenance run --task=gc --task=gc 2>err &&
	test_i18ngrep "cannot be selected multiple times" err
'

test_expect_success 'run --task=fetch with no remotes' '
	git maintenance run --task=fetch 2>err &&
	test_must_be_empty err
'

test_expect_success 'fetch multiple remotes' '
	git clone . clone1 &&
	git clone . clone2 &&
	git remote add remote1 "file://$(pwd)/clone1" &&
	git remote add remote2 "file://$(pwd)/clone2" &&
	git -C clone1 switch -c one &&
	git -C clone2 switch -c two &&
	test_commit -C clone1 one &&
	test_commit -C clone2 two &&
	GIT_TRACE2_EVENT="$(pwd)/run-fetch.txt" git maintenance run --task=fetch &&
	grep ",\"fetch\",\"remote1\"" run-fetch.txt &&
	grep ",\"fetch\",\"remote2\"" run-fetch.txt &&
	test_path_is_missing .git/refs/remotes &&
	test_cmp clone1/.git/refs/heads/one .git/refs/hidden/remote1/one &&
	test_cmp clone2/.git/refs/heads/two .git/refs/hidden/remote2/two &&
	git log hidden/remote1/one &&
	git log hidden/remote2/two
'

test_expect_success 'loose-objects task' '
	# Repack everything so we know the state of the object dir
	git repack -adk &&

	# Hack to stop maintenance from running during "git commit"
	echo in use >.git/objects/maintenance.lock &&
	test_commit create-loose-object &&
	rm .git/objects/maintenance.lock &&

	ls .git/objects >obj-dir-before &&
	test_file_not_empty obj-dir-before &&
	ls .git/objects/pack/*.pack >packs-before &&
	test_line_count = 1 packs-before &&

	# The first run creates a pack-file
	# but does not delete loose objects.
	git maintenance run --task=loose-objects &&
	ls .git/objects >obj-dir-between &&
	test_cmp obj-dir-before obj-dir-between &&
	ls .git/objects/pack/*.pack >packs-between &&
	test_line_count = 2 packs-between &&

	# The second run deletes loose objects
	# but does not create a pack-file.
	git maintenance run --task=loose-objects &&
	ls .git/objects >obj-dir-after &&
	cat >expect <<-\EOF &&
	info
	pack
	EOF
	test_cmp expect obj-dir-after &&
	ls .git/objects/pack/*.pack >packs-after &&
	test_cmp packs-between packs-after
'

test_expect_success 'maintenance.loose-objects.auto' '
	git repack -adk &&
	GIT_TRACE2_EVENT="$(pwd)/trace-lo1.txt" \
		git -c maintenance.loose-objects.auto=1 maintenance \
		run --auto --task=loose-objects &&
	! grep "\"prune-packed\"" trace-lo1.txt &&
	for i in 1 2
	do
		printf data-A-$i | git hash-object -t blob --stdin -w &&
		GIT_TRACE2_EVENT="$(pwd)/trace-loA-$i" \
			git -c maintenance.loose-objects.auto=2 \
			maintenance run --auto --task=loose-objects &&
		! grep "\"prune-packed\"" trace-loA-$i &&
		printf data-B-$i | git hash-object -t blob --stdin -w &&
		GIT_TRACE2_EVENT="$(pwd)/trace-loB-$i" \
			git -c maintenance.loose-objects.auto=2 \
			maintenance run --auto --task=loose-objects &&
		grep "\"prune-packed\"" trace-loB-$i &&
		GIT_TRACE2_EVENT="$(pwd)/trace-loC-$i" \
			git -c maintenance.loose-objects.auto=2 \
			maintenance run --auto --task=loose-objects &&
		grep "\"prune-packed\"" trace-loC-$i || return 1
	done
'

test_expect_success 'pack-files task' '
	packDir=.git/objects/pack &&
	for i in $(test_seq 1 5)
	do
		test_commit $i || return 1
	done &&

	# Create three disjoint pack-files with size BIG, small, small.
	echo HEAD~2 | git pack-objects --revs $packDir/test-1 &&
	test_tick &&
	git pack-objects --revs $packDir/test-2 <<-\EOF &&
	HEAD~1
	^HEAD~2
	EOF
	test_tick &&
	git pack-objects --revs $packDir/test-3 <<-\EOF &&
	HEAD
	^HEAD~1
	EOF
	rm -f $packDir/pack-* &&
	rm -f $packDir/loose-* &&
	ls $packDir/*.pack >packs-before &&
	test_line_count = 3 packs-before &&

	# the job repacks the two into a new pack, but does not
	# delete the old ones.
	git maintenance run --task=pack-files &&
	ls $packDir/*.pack >packs-between &&
	test_line_count = 4 packs-between &&

	# the job deletes the two old packs, and does not write
	# a new one because the batch size is not high enough to
	# pack the largest pack-file.
	git maintenance run --task=pack-files &&
	ls .git/objects/pack/*.pack >packs-after &&
	test_line_count = 2 packs-after
'

test_expect_success 'maintenance.pack-files.auto' '
	git repack -adk &&
	git config core.multiPackIndex true &&
	git multi-pack-index write &&
	GIT_TRACE2_EVENT=1 git -c maintenance.pack-files.auto=1 maintenance \
		run --auto --task=pack-files >out &&
	! grep "\"multi-pack-index\"" out &&
	for i in 1 2
	do
		test_commit A-$i &&
		git pack-objects --revs .git/objects/pack/pack <<-\EOF &&
		HEAD
		^HEAD~1
		EOF
		GIT_TRACE2_EVENT=$(pwd)/trace-A-$i git \
			-c maintenance.pack-files.auto=2 \
			maintenance run --auto --task=pack-files &&
		! grep "\"multi-pack-index\"" trace-A-$i &&
		test_commit B-$i &&
		git pack-objects --revs .git/objects/pack/pack <<-\EOF &&
		HEAD
		^HEAD~1
		EOF
		GIT_TRACE2_EVENT=$(pwd)/trace-B-$i git \
			-c maintenance.pack-files.auto=2 \
			maintenance run --auto --task=pack-files >out &&
		grep "\"multi-pack-index\"" trace-B-$i >/dev/null || return 1
	done
'

test_done
