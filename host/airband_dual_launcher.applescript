on checkBoard(ip)
	try
		set hdr to do shell script "/usr/bin/nc -G 3 " & ip & " 1234 </dev/null | /usr/bin/head -c 4"
		return hdr is "RTL0"
	on error
		return false
	end try
end checkBoard

on launchGqrx(confName, ip)
	if my checkBoard(ip) then
		do shell script "sed -i '' 's/crashed=true/crashed=false/' \"$HOME/.config/gqrx/" & confName & "\"; nohup /opt/homebrew/bin/gqrx -c " & confName & " >/dev/null 2>&1 &"
		delay 3
		return ""
	else
		return ip & " が応答しません（PoE と LED を確認）" & return
	end if
end launchGqrx

on run
	set btn to button returned of (display dialog "エアバンド受信 ×2" & return & "1号機 10.5.2.20 / 5号機 10.5.2.24" buttons {"停止", "片方だけ", "両方起動"} default button "両方起動" with title "エアバンド受信×2")
	set msg to ""
	if btn is "両方起動" then
		set msg to msg & my launchGqrx("default.conf", "10.5.2.20")
		set msg to msg & my launchGqrx("board5.conf", "10.5.2.24")
	else if btn is "片方だけ" then
		set which to button returned of (display dialog "どちらを起動しますか？" buttons {"閉じる", "5号機 (10.5.2.24)", "1号機 (10.5.2.20)"} default button "1号機 (10.5.2.20)" with title "エアバンド受信×2")
		if which starts with "1号機" then set msg to my launchGqrx("default.conf", "10.5.2.20")
		if which starts with "5号機" then set msg to my launchGqrx("board5.conf", "10.5.2.24")
	else if btn is "停止" then
		do shell script "pkill -f -i gqrx; true"
	end if
	if msg is not "" then display dialog msg buttons {"OK"} default button "OK" with icon caution with title "エアバンド受信×2"
end run
