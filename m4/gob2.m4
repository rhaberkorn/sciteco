dnl
dnl GOB_HOOK(script if found, fail)
dnl if fail = "failure", abort if GOB not found
dnl


AC_DEFUN([GOB2_HOOK],[
	AC_PATH_PROG(GOB2,gob2)
	if test ! x$GOB2 = x; then	
		if test ! x$1 = x; then 
			AC_MSG_CHECKING(for gob-2 >= $1)
			g_r_ve=`echo $1|sed 's/\([[0-9]]*\).\([[0-9]]*\).\([[0-9]]*\)/\1/'`
			g_r_ma=`echo $1|sed 's/\([[0-9]]*\).\([[0-9]]*\).\([[0-9]]*\)/\2/'`
			g_r_mi=`echo $1|sed 's/\([[0-9]]*\).\([[0-9]]*\).\([[0-9]]*\)/\3/'`
			g_ve=`$GOB2 --version 2>&1|sed 's/Gob version \([[0-9]]*\).\([[0-9]]*\).\([[0-9]]*\)/\1/'`
			g_ma=`$GOB2 --version 2>&1|sed 's/Gob version \([[0-9]]*\).\([[0-9]]*\).\([[0-9]]*\)/\2/'`
			g_mi=`$GOB2 --version 2>&1|sed 's/Gob version \([[0-9]]*\).\([[0-9]]*\).\([[0-9]]*\)/\3/'`

			if test $g_ve -eq $g_r_ve; then
				if test $g_ma -ge $g_r_ma; then
					if test $g_mi -ge $g_r_mi; then
						AC_MSG_RESULT(ok)
					else
						if test $g_ma -gt $g_r_ma; then
							AC_MSG_RESULT(ok)
						else
							AC_MSG_ERROR("found $g_ve.$g_ma.$g_mi requires $g_r_ve.$g_r_ma.$g_r_mi")
						fi
					fi
				else
					AC_MSG_ERROR("found $g_ve.$g_ma.$g_mi requires $g_r_ve.$g_r_ma.$g_r_mi")
				fi
			else
				if test $g_ve -gt $g_r_ve; then
					AC_MSG_RESULT(ok)
				else
					AC_MSG_ERROR(major version $g_ve found but $g_r_ve required)
				fi
			fi
	
			unset gob_version
			unset g_ve
			unset g_ma
			unset g_mi
			unset g_r_ve
			unset g_r_ma
			unset g_r_mi
		fi
		AC_SUBST(GOB2)
		$2
	else		
		$3
	fi
])

AC_DEFUN([GOB2_CHECK],[
	GOB2_HOOK($1,[],[AC_MSG_ERROR([Cannot find GOB-2, check http://www.5z.com/jirka/gob.html])])
])
