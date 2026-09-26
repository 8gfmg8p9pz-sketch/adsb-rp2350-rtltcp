on run
	set btn to button returned of (display dialog "RP2350 エアバンド受信" & return & "ボード 10.5.2.20 / 118.1 MHz AM" buttons {"停止", "閉じる", "起動"} default button "起動" with title "エアバンド受信")
	if btn is "起動" then
		set hdr to ""
		try
			set hdr to do shell script "/usr/bin/nc -G 3 10.5.2.20 1234 </dev/null | /usr/bin/head -c 4"
		end try
		if hdr is not "RTL0" then
			display dialog "ボード (10.5.2.20) に接続できません。" & return & "PoE の LAN ケーブルと、ボードの LED（青）を確認してください。" buttons {"OK"} default button "OK" with icon caution with title "エアバンド受信"
			return
		end if
		do shell script "pkill -f -i gqrx; sleep 1; sed -i '' 's/crashed=true/crashed=false/' \"$HOME/.config/gqrx/default.conf\"; nohup /opt/homebrew/bin/gqrx -c default.conf >/dev/null 2>&1 &"
	else if btn is "停止" then
		do shell script "pkill -f -i gqrx; true"
	end if
end run
