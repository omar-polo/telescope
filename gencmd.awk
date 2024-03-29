BEGIN {
	FS = "[(,)]";

	print "#include \"compat.h\""
	print "#include \"cmd.h\""
	print "struct cmd cmds[] = {";
}

/^CMD/ {
	s = $2;
	sub("^cmd_", "", s);
	gsub("_", "-", s);
	printf("\t{ \"%s\", %s, %s },\n", s, $2, $3);
	next;
}

/^DEFALIAS/ {
	s = $2;
	d = $3;
	printf("\t{ \"%s\", %s, NULL },\n", s, d);
	next
}

{
	next;
}

END {
	printf("\t{ NULL, NULL, NULL },\n");
	print "};";
}
