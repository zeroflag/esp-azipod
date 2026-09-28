WIFI load
NETCON load

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

-1 init-variable: angle

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

-1 init-variable: throttle

: potmeter-read ( -- n )
  adc-read
  8 1 do
    adc-read +
  loop
  3 rshift ; \ div by 8

( *** Networking *** )

\ High 16 bit: Azimut angle
\ Low  16 bit: Throttle turn
4 buffer: packet

"192.168.0.151" 6589 
  constant: SERVER_PORT
  constant: SERVER_IP

: make-packet ( -- )
  angle   @ 16 lshift
  throttle @ 16rFFFF and
  or
  packet ! ;

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

: main ( -- )
  as5600-init
  poll-sensors ;

main
