#
# Regular cron jobs for the weston package.
#
0 4	* * *	root	[ -x /usr/bin/weston_maintenance ] && /usr/bin/weston_maintenance
