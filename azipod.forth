WIFI load
NETCON load

FALSE constant: STARBOARD

( *** I2C AS5600 sensor *** )

5 ( D1 SCL ) constant: SCL
4 ( D2 SDA ) constant: SDA
16r36        constant: I2C-SLAVE
0            constant: I2C-BUS
2 ( 400K )   constant: I2C-FREQ

exception: EI2C

2 buffer:  as5600-buf
1 buffer:  as5600-reg-angle
1 buffer:  as5600-reg-status

16r0E as5600-reg-angle  c!
16r0B as5600-reg-status c!

180 init-variable: angle

: i2c-check ( code -- | throws:EI2C ) 0<> if EI2C throw then ;

: as5600-init ( -- ) I2C-FREQ SDA SCL I2C-BUS i2c-init i2c-check ;

: as5600-read-register ( len as5600-buf reg-addr -- )
  I2C-SLAVE I2C-BUS i2c-read-slave i2c-check ;

: as5600-read-status ( -- n )
  1 as5600-buf as5600-reg-status as5600-read-register
  as5600-buf c@ ;

\ bit:   7 6  5   4   3  2 1 0
\        - -  MD  ML MH  - - -
: as5600-magnet-detected? ( -- bool ) as5600-read-status 32 and 0<> ;

: as5600-read-angle ( -- n )
  2 as5600-buf as5600-reg-angle as5600-read-register
  as5600-buf    c@ 8 lshift
  as5600-buf 1+ c@   or
  360 *  12   rshift ;

( *** Potentiometer analog input *** )

0 init-variable: throttle

: potmeter-read ( -- n )
  adc-read
  8 1 do
    adc-read +
  loop
  3 rshift ; \ div by 8

( *** Networking *** )

\ Low  0  -  9 bit (10): Throttle turn (0-1023)
\ Mid  10 - 18 bit ( 9): Angle (0-359)
\ Mid  19 - 30 bit (12): Sequence number
\ High 31      bit (1): 1 if Starboard 0 if port
\ F|SSSSSSSSSSSS|AAAAAAAAA|TTTTTTTTTT
4 buffer: packet

0 init-variable: sequence

"192.168.0.151" 6589
  constant: SERVER_PORT
  constant: SERVER_IP

: flag ( n -- n )
  STARBOARD if
    16r80000000 or
  else
    16r7FFFFFFF and
  then ;

: inc-sequence ( -- )
  sequence @ 1+ 16rFFF and sequence ! ;

: make-packet ( -- )
  sequence @ 16rFFF and 19 lshift
  angle    @ 16r1FF and 10 lshift or
  throttle @ 16r3FF and or
  flag
  packet !
  inc-sequence ;

: send ( -- )
  make-packet
  SERVER_PORT SERVER_IP UDP netcon-connect
  dup packet 4 netcon-send-buf
  netcon-dispose ;

( *** Main *** )

3  constant: MIN_CHANGE
20 constant: DELAY ( ms )

variable: to-send

: changed? ( a b -- bool ) - abs MIN_CHANGE >= ;

: poll-potmeter ( -- )
  potmeter-read
  dup throttle @ changed? if
    print: "Throttle: "
    dup . cr
    throttle !
    TRUE to-send !
  else
    drop
  then ;

: poll-as5600 ( -- )    
  as5600-magnet-detected? if
    as5600-read-angle
    dup angle @ <> if
      angle !
      print: "Angle: "
      angle @ . cr
      TRUE to-send !
    else
      drop
    then
  then ;

: poll-sensors ( -- )
  begin
    FALSE to-send !
    poll-potmeter
    poll-as5600
    to-send @ if send then
    DELAY ms
  again ;

: name ( -- s )
  STARBOARD if
    "Starboard Azipod"
  else
    "Port Azipod"
  then ;

: main ( -- )
  cr name type
  println: " ready."
  as5600-init
  poll-sensors ;

main
