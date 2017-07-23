#!/bin/bash
# create multiresolution windows icon
ICON_DST=../../src/qt/res/icons/neutron.ico

convert ../../src/qt/res/icons/neutron-16.png ../../src/qt/res/icons/neutron-32.png ../../src/qt/res/icons/neutron-48.png ${ICON_DST}
