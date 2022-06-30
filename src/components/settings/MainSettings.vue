<template>
  <v-layout
    row
    justify-start
    wrap
    align-baseline
  >
    <v-flex
      v-for="param of params"
      :key="param.id"
      xs12
      sm6
      pa-2
    >
      <app-select-action
        v-if="param.type == valueTypes.discrete"
        :param="param"
        :action-disabled="openCvLoading"
        @select-change="value => $emit('changed', value)"
        @action-click="$emit('load-opencv')"
      />

      <v-slider
        v-if="param.type == valueTypes.rangeSquareRoot"  
        always-dirty
        :value="sliderValueSqrt(param.value)"
        :hint="`${paramName(param.id)}: ${param.value} px`"
        persistent-hint
        :max="sliderValueSqrt(param.range.max)"
        :min="sliderValueSqrt(param.range.min)"
        :step="20"
        :style="{'margin-top': '0', 'margin-bottom': '0'}"
        @change="value => $emit('changed', { id: param.id, value: value * value })"
      />
    </v-flex>
  </v-layout>
</template>

<script>

import { ParamUtils, valueTypes } from '@/models/constants/params';
import SelectAction from '@/components/settings/SelectAction';

export default {
  components: {
    'AppSelectAction': SelectAction
  },
  props: {
    params: {
      type: Array,
      required: true
    },
    openCvReady: {
      type: Boolean,
      required: true
    },
    openCvLoading: {
      type: Boolean,
      required: true
    }
  },
  computed: {
    valueTypes() {
      return valueTypes;
    }
  },
  methods: {
    paramName(id) {
      return ParamUtils.getParamName(id);
    },
    paramText(id) {
      return ParamUtils.getParamText(id);
    },
    sliderValueSqrt(value) {
      return Math.round(Math.sqrt(value));
    }
  }
}
</script>

<style scoped>

</style>
