# SPDX-License-Identifier: GPL-2.0-or-later
"""Guard the colour-only correction after the runtime highlight regression."""
import unittest
import numpy as np
import tone

class PaletteFixture:
    def __init__(self):
        values=np.arange(256,dtype=np.uint16)
        self.pal=np.stack([values,values,values],axis=1)

class ToneGuards(unittest.TestCase):
    def test_output_colours_stay_in_original_action_palette(self):
        pal=PaletteFixture();source=np.repeat([20,40,60,80],[20,40,30,10]);candidate=np.repeat([0,15,80,250],[20,40,30,10])
        lut,curve=tone.tone_lut(source,candidate,pal)
        self.assertTrue(set(lut.tolist()).issubset(set(source.tolist())))
        self.assertTrue(np.all(np.diff(curve['candidate_L_knots'])>0))
        self.assertTrue(np.all(np.diff(curve['source_L_targets'])>=0))
        self.assertEqual(lut[candidate].max(),source.max())
        self.assertEqual(lut[candidate].min(),source.min())
    def test_same_sequence_distribution_has_one_deterministic_mapping(self):
        pal=PaletteFixture();source=np.array([20,40,60,80,40]);candidate=np.array([0,20,140,250,20])
        a,_=tone.tone_lut(source,candidate,pal);b,_=tone.tone_lut(source[::-1],candidate[::-1],pal)
        np.testing.assert_array_equal(a,b)
        np.testing.assert_array_equal(a[candidate],b[candidate])
    def test_single_original_tone_does_not_create_contrast(self):
        pal=PaletteFixture();lut,_=tone.tone_lut(np.full(10,40),np.array([0,50,250]),pal)
        np.testing.assert_array_equal(lut,np.full(256,40))

if __name__=='__main__':unittest.main()
