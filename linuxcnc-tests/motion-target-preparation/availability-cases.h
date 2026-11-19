static void test_availability(EmcPose *pose, double *positions, CUBIC_STRUCT *staged) {
    availability_status_t *a = &status.joint_status[2].availability;
    a->controlled = 1; a->held_position = 2;
    unsigned char before[sizeof(joints)]; memcpy(before, joints, sizeof(joints));
    int saved_fflags = fflags;
    solution[0] = 10; solution[2] = 3;
    assert(prepare_joint_targets(pose, positions, staged) != 0);
    assert(memcmp(before, joints, sizeof(joints)) == 0);
    assert(fflags == saved_fflags);
    solution[2] = 2;
    assert(prepare_joint_targets(pose, positions, staged) == 0);
    assert(fflags == saved_fflags + 1);
    assert(positions[0] == 10 && positions[2] == 2);
    for (int n=1; n<100; n++) {
        solution[2] = 2 + n*1e-10;
        int result = prepare_joint_targets(pose, positions, staged);
        if (n <= 9) { assert(result == 0); assert(positions[2] == 2); }
        if (n >= 11) assert(result != 0);
        assert(a->held_position == 2);
    }
}
